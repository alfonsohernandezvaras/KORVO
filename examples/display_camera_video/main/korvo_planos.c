/* KORVO_PLANOS01 - PDF repository; no PDF decoding or rendering. */
#include "korvo_planos.h"
#include "korvo_storage.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#define ROOT BSP_SD_MOUNT_POINT "/korvo/planos equipamiento"
#define MAX_PATH 384
#define MAX_UPLOAD (32U*1024U*1024U)
static bool (*s_auth)(httpd_req_t *) = NULL;
static bool (*s_admin)(httpd_req_t *) = NULL; /* PLANOS03 */
static bool s_busy = false;
static const char *TAG="korvo_planos";

static esp_err_t reply(httpd_req_t *r,const char *status,const char *msg){
 httpd_resp_set_status(r,status);httpd_resp_set_type(r,"text/plain; charset=utf-8");return httpd_resp_sendstr(r,msg);
}
static bool access_ok(httpd_req_t *r){if(s_auth && s_auth(r))return true;reply(r,"403 Forbidden","Acceso no autorizado");return false;}
/* PLANOS03: no confiar en botones ocultos; validar cada escritura en servidor. */
static bool write_ok(httpd_req_t *r){
 if(s_admin && s_admin(r))return true;
 reply(r,"403 Forbidden","Solo administrador puede modificar planos");
 return false;
}
static esp_err_t role_g(httpd_req_t *r){
 if(!access_ok(r))return ESP_OK;
 httpd_resp_set_type(r,"application/json; charset=utf-8");
 httpd_resp_set_hdr(r,"Cache-Control","no-store");
 return httpd_resp_sendstr(r,(s_admin&&s_admin(r))?"{\"admin\":true}":"{\"admin\":false}");
}
static bool decode(const char *in,char *out,size_t cap){
 size_t j=0;for(size_t i=0;in[i];i++){
  unsigned char c=(unsigned char)in[i];if(c=='%'){
   if(!isxdigit((unsigned char)in[i+1])||!isxdigit((unsigned char)in[i+2]))return false;
   char h[3]={in[i+1],in[i+2],0};c=(unsigned char)strtoul(h,NULL,16);i+=2;
  }else if(c=='+')c=' ';
  if(c<32||c==127||j+1>=cap)return false;
  out[j++]=(char)c;
 }out[j]=0;return true;
}
static bool query_path(httpd_req_t *r,char *relative,size_t n){
 char q[512],raw[384];size_t len=httpd_req_get_url_query_len(r);
 if(len==0){relative[0]=0;return true;}
 if(len>=sizeof(q)||httpd_req_get_url_query_str(r,q,sizeof(q))!=ESP_OK)return false;
 if(httpd_query_key_value(q,"path",raw,sizeof(raw))!=ESP_OK)return false;
 return decode(raw,relative,n);
}
static bool safe_path(const char *rel,char *dst,size_t n){
 if(rel[0]=='/'||rel[0]=='\\'||strchr(rel,'\\')||strchr(rel,':')||strstr(rel,"..")||strstr(rel,"//"))return false;
 for(const unsigned char *p=(const unsigned char*)rel;*p;p++)if(*p<32||*p==127)return false;
 int z=snprintf(dst,n,"%s%s%s",ROOT,rel[0]?"/":"",rel);return z>0&&(size_t)z<n;
}
static bool pdf_name(const char *s){size_t n=strlen(s);return n>4&&strcasecmp(s+n-4,".pdf")==0;}
static void json_str(httpd_req_t *r,const char *s){
 httpd_resp_send_chunk(r,"\"",1);char b[8];for(const unsigned char *p=(const unsigned char*)s;*p;p++){
  if(*p=='"'||*p=='\\'){b[0]='\\';b[1]=*p;httpd_resp_send_chunk(r,b,2);}
  else if(*p<32){snprintf(b,sizeof(b),"\\u%04x",*p);httpd_resp_send_chunk(r,b,6);}
  else httpd_resp_send_chunk(r,(const char*)p,1);
 }httpd_resp_send_chunk(r,"\"",1);
}
static esp_err_t tree_g(httpd_req_t *r){
 if(!access_ok(r))return ESP_OK;
 char rel[320],path[MAX_PATH];if(!query_path(r,rel,sizeof(rel))||!safe_path(rel,path,sizeof(path)))return reply(r,"400 Bad Request","Ruta invalida");
 DIR *d=opendir(path);if(!d)return reply(r,"404 Not Found","Carpeta no encontrada");
 httpd_resp_set_type(r,"application/json; charset=utf-8");httpd_resp_send_chunk(r,"[",1);
 struct dirent *e;bool first=true;unsigned count=0;
 while((e=readdir(d))!=NULL&&count<250){
  if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")||e->d_name[0]=='.')continue;
  char full[MAX_PATH];if(snprintf(full,sizeof(full),"%s/%s",path,e->d_name)>=(int)sizeof(full))continue;
  struct stat st;if(stat(full,&st)!=0)continue;
  if(!S_ISDIR(st.st_mode)&&!(S_ISREG(st.st_mode)&&pdf_name(e->d_name)))continue;
  if(!first)httpd_resp_send_chunk(r,",",1);
  first=false;
  count++;
  httpd_resp_send_chunk(r,"{\"name\":",8);json_str(r,e->d_name);
  const char *kind=S_ISDIR(st.st_mode)?",\"dir\":true,\"size\":" : ",\"dir\":false,\"size\":";httpd_resp_send_chunk(r,kind,strlen(kind));
  char num[32];snprintf(num,sizeof(num),"%lu}",(unsigned long)(S_ISDIR(st.st_mode)?0:st.st_size));httpd_resp_send_chunk(r,num,strlen(num));
 }
 closedir(d);httpd_resp_send_chunk(r,"]",1);return httpd_resp_send_chunk(r,NULL,0);
}
static esp_err_t download_g(httpd_req_t *r){
 if(!access_ok(r))return ESP_OK;
 char rel[320],path[MAX_PATH];if(!query_path(r,rel,sizeof(rel))||!rel[0]||!safe_path(rel,path,sizeof(path))||!pdf_name(rel))return reply(r,"400 Bad Request","PDF invalido");
 struct stat st;if(stat(path,&st)!=0||!S_ISREG(st.st_mode))return reply(r,"404 Not Found","No existe");
 if(s_busy)return reply(r,"503 Service Unavailable","Transferencia en curso");
 s_busy=true;
 FILE *f=fopen(path,"rb");if(!f){s_busy=false;return reply(r,"500 Internal Server Error","No se pudo leer SD");}
 unsigned char magic[5];bool valid=fread(magic,1,5,f)==5&&!memcmp(magic,"%PDF-",5);rewind(f);
 if(!valid){fclose(f);s_busy=false;return reply(r,"415 Unsupported Media Type","Archivo no es PDF");}
 const char *name=strrchr(rel,'/');name=name?name+1:rel;
 char disp[420];char cleaned[180];size_t j=0;
 for(size_t i=0;name[i]&&j+1<sizeof(cleaned);i++){unsigned char c=name[i];cleaned[j++]=(c=='"'||c=='\\'||c<32||c>126)?'_':c;}cleaned[j]=0;
 snprintf(disp,sizeof(disp),"attachment; filename=\"%s\"",cleaned);
 httpd_resp_set_type(r,"application/pdf");httpd_resp_set_hdr(r,"Content-Disposition",disp);httpd_resp_set_hdr(r,"Cache-Control","no-store");
 char buf[4096];size_t n;esp_err_t result=ESP_OK;
 while((n=fread(buf,1,sizeof(buf),f))>0){if(httpd_resp_send_chunk(r,buf,n)!=ESP_OK){result=ESP_FAIL;break;}}
 if(ferror(f))result=ESP_FAIL;
 fclose(f);
 if(result==ESP_OK)result=httpd_resp_send_chunk(r,NULL,0);
 s_busy=false;
 return result;
}
static esp_err_t upload_p(httpd_req_t *r){
 if(!write_ok(r))return ESP_OK;
 if(s_busy)return reply(r,"503 Service Unavailable","Transferencia en curso");
 if(r->content_len<=0||(unsigned)r->content_len>MAX_UPLOAD)return reply(r,"413 Payload Too Large","Maximo 32 MB");
 char rel[320],path[MAX_PATH];if(!query_path(r,rel,sizeof(rel))||!rel[0]||!safe_path(rel,path,sizeof(path))||!pdf_name(rel))return reply(r,"400 Bad Request","Nombre PDF invalido");
 struct stat st;if(stat(path,&st)==0)return reply(r,"409 Conflict","Archivo existente: no se sobrescribe");
 char parent[MAX_PATH];strcpy(parent,path);char *slash=strrchr(parent,'/');if(!slash)return reply(r,"400 Bad Request","Ruta invalida");*slash=0;
 if(stat(parent,&st)!=0||!S_ISDIR(st.st_mode))return reply(r,"404 Not Found","Carpeta destino no existe");
 char tmp[MAX_PATH];if(snprintf(tmp,sizeof(tmp),"%s/.planos_upload.tmp",ROOT)>=(int)sizeof(tmp))return reply(r,"500 Internal Server Error","Ruta larga");
 s_busy=true;FILE *f=fopen(tmp,"wb");if(!f){s_busy=false;return reply(r,"507 Insufficient Storage","SD no disponible");}
 char buf[4096];int remain=r->content_len;bool ok=true;bool first=true;
 while(remain>0){int want=remain>(int)sizeof(buf)?sizeof(buf):remain;int n=httpd_req_recv(r,buf,want);if(n<=0){ok=false;break;}
  if(first){if(n<5||memcmp(buf,"%PDF-",5)!=0){ok=false;break;}first=false;}
  if(fwrite(buf,1,n,f)!=(size_t)n){ok=false;break;}remain-=n;
 }
 if(fclose(f)!=0)ok=false;
 if(ok&&rename(tmp,path)!=0)ok=false;
 if(!ok)remove(tmp);
 s_busy=false;
 return ok?reply(r,"201 Created","PDF almacenado correctamente"):reply(r,"400 Bad Request","Carga incompleta o PDF invalido");
}
static esp_err_t mkdir_p(httpd_req_t *r){
 if(!write_ok(r))return ESP_OK;
 if(s_busy)return reply(r,"503 Service Unavailable","Transferencia en curso");
 char rel[320],path[MAX_PATH];if(!query_path(r,rel,sizeof(rel))||!rel[0]||!safe_path(rel,path,sizeof(path))||pdf_name(rel))return reply(r,"400 Bad Request","Nombre invalido");
 char parent[MAX_PATH];strcpy(parent,path);char *slash=strrchr(parent,'/');if(!slash)return reply(r,"400 Bad Request","Ruta invalida");*slash=0;
 struct stat st;if(stat(parent,&st)!=0||!S_ISDIR(st.st_mode))return reply(r,"404 Not Found","Carpeta padre no existe");
 if(mkdir(path,0775)!=0)return reply(r,errno==EEXIST?"409 Conflict":"500 Internal Server Error","No se pudo crear carpeta");
 return reply(r,"201 Created","Carpeta creada");
}
/* PLANOS02: acciones SD limitadas estrictamente a ROOT. */
static bool param_path(httpd_req_t *r,const char *key,char *rel,size_t cap){
 char query[900],raw[450];size_t n=httpd_req_get_url_query_len(r);
 if(!n||n>=sizeof(query)||httpd_req_get_url_query_str(r,query,sizeof(query))!=ESP_OK)return false;
 if(httpd_query_key_value(query,key,raw,sizeof(raw))!=ESP_OK)return false;
 return decode(raw,rel,cap);
}
static esp_err_t delete_p(httpd_req_t *r){
 if(!write_ok(r))return ESP_OK;
 if(s_busy)return reply(r,"503 Service Unavailable","Transferencia en curso");
 char rel[320],path[MAX_PATH];struct stat st;
 if(!param_path(r,"path",rel,sizeof(rel))||!rel[0]||!safe_path(rel,path,sizeof(path)))return reply(r,"400 Bad Request","Ruta invalida");
 if(stat(path,&st)!=0)return reply(r,"404 Not Found","No existe");
 if(S_ISREG(st.st_mode)&&pdf_name(rel)){
  if(remove(path)!=0)return reply(r,"500 Internal Server Error","No se pudo eliminar PDF");
 }else if(S_ISDIR(st.st_mode)){
  if(rmdir(path)!=0)return reply(r,"409 Conflict","La carpeta debe estar vacia");
 }else return reply(r,"403 Forbidden","Tipo de archivo no permitido");
 return reply(r,"200 OK","Eliminado");
}
static esp_err_t move_p(httpd_req_t *r){
 if(!write_ok(r))return ESP_OK;
 if(s_busy)return reply(r,"503 Service Unavailable","Transferencia en curso");
 char from[320],to[320],src[MAX_PATH],dst[MAX_PATH];struct stat st;
 if(!param_path(r,"from",from,sizeof(from))||!param_path(r,"to",to,sizeof(to))||!from[0]||!to[0]||!safe_path(from,src,sizeof(src))||!safe_path(to,dst,sizeof(dst)))return reply(r,"400 Bad Request","Ruta invalida");
 if(stat(src,&st)!=0||!S_ISREG(st.st_mode)||!pdf_name(from)||!pdf_name(to))return reply(r,"400 Bad Request","Solo se pueden mover PDF");
 if(stat(dst,&st)==0)return reply(r,"409 Conflict","Ya existe un archivo con ese nombre");
 char parent[MAX_PATH];strcpy(parent,dst);char *slash=strrchr(parent,'/');if(!slash)return reply(r,"400 Bad Request","Ruta invalida");*slash=0;
 if(stat(parent,&st)!=0||!S_ISDIR(st.st_mode))return reply(r,"404 Not Found","Carpeta destino inexistente");
 if(rename(src,dst)!=0)return reply(r,"500 Internal Server Error","No se pudo mover");
 return reply(r,"200 OK","Archivo movido");
}

/* KORVO_PLANOS04: seleccion de archivos y carpetas para eliminar. */
static const char PAGE[] =
"<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Planos Equipamiento</title>"
"<style>*{box-sizing:border-box}body{font:14px Arial;background:#10151b;color:#e9eef4;margin:0}header{background:#171f28;padding:12px;display:flex;justify-content:space-between}main{padding:15px;max-width:1050px}a{color:#8ecbff}button,input,select{padding:8px;background:#202c37;color:white;border:1px solid #445363;border-radius:5px}button{cursor:pointer}button:disabled{opacity:.45}section{background:#18212b;padding:14px;margin:12px 0;border-radius:8px}.item{display:flex;align-items:center;gap:10px;padding:9px;border-bottom:1px solid #34404d}.item:hover{background:#253442}.name{flex:1;cursor:pointer;color:#8ecbff;overflow-wrap:anywhere}.muted{color:#9eabb8;font-size:12px}.toolbar{display:flex;gap:7px;flex-wrap:wrap;align-items:center}.crumb{margin:14px 0;color:#8ecbff}.danger{color:#ff9a9a}#msg{display:block;margin-top:10px}input[type=file]{max-width:320px}input[type=checkbox]{width:17px;height:17px}</style></head>"
"<body><header><b>PLANOS EQUIPAMIENTO</b><nav><a id=\"home\" href=\"/\">Estado</a> &nbsp; <a href=\"/logout\">Salir</a></nav></header><main>"
"<h2>Repositorio documental de la sala</h2><p class=\"muted\">Los PDF se descargan directamente al PC, sin visor.</p>"
"<section id=\"adminTools\" hidden><div class=\"toolbar\"><input id=\"file\" type=\"file\" accept=\".pdf,application/pdf\" multiple><button onclick=\"upload()\">Subir PDF</button><button onclick=\"createFolder()\">Nueva carpeta</button><button onclick=\"moveSelected()\">Mover seleccionados</button><button onclick=\"deleteSelected()\">Eliminar seleccionados</button></div></section><section><div class=\"toolbar\"><button onclick=\"refresh()\">Actualizar</button><small id=\"msg\" class=\"muted\"></small></div></section>"
"<section><div class=\"crumb\" id=\"crumb\"></div><div class=\"toolbar\"><button id=\"back\" onclick=\"up()\">↑ Carpeta anterior</button><label id=\"selectTools\" class=\"muted\" hidden><input id=\"all\" type=\"checkbox\" onchange=\"toggleAll(this.checked)\"> Seleccionar todo</label></div><div id=\"items\">Cargando...</div></section>"
"</main><script>"
"const api='/api/planos/';let dir='',items=[],isAdmin=false;const selected=new Set();"
"const enc=s=>encodeURIComponent(s);const join=(a,b)=>a?a+'/'+b:b;"
"function msg(s){document.getElementById('msg').textContent=s}"
"async function request(op,path,method='GET',body){const r=await fetch(api+op+'?path='+enc(path),{method,body});const t=await r.text();if(!r.ok)throw Error(t);return t}"
"async function refresh(){selected.clear();document.getElementById('all').checked=false;document.getElementById('crumb').textContent='Planos Equipamiento / '+dir;document.getElementById('back').disabled=!dir;const host=document.getElementById('items');host.textContent='Cargando...';try{const r=await fetch(api+'tree?path='+enc(dir));if(!r.ok)throw Error(await r.text());items=await r.json();items.sort((a,b)=>Number(b.dir)-Number(a.dir)||a.name.localeCompare(b.name));host.replaceChildren();if(!items.length){host.textContent='Carpeta vacía';return}for(const x of items){const p=join(dir,x.name),row=document.createElement('div');row.className='item';if(isAdmin){const cb=document.createElement('input');cb.type='checkbox';cb.dataset.path=p;cb.onchange=()=>{if(cb.checked)selected.add(p);else selected.delete(p);syncAll()};row.appendChild(cb)}else{const space=document.createElement('span');space.textContent=x.dir?'📁':'📄';row.appendChild(space)}const name=document.createElement('span');name.className='name';name.textContent=x.name;name.onclick=()=>{if(x.dir){dir=p;refresh()}else location.href=api+'download?path='+enc(p)};row.appendChild(name);const size=document.createElement('span');size.className='muted';size.textContent=x.dir?'Carpeta':Math.round(x.size/1024)+' KB';row.appendChild(size);host.appendChild(row)}}catch(e){host.textContent='Error: '+e.message}}"
"function up(){if(dir){dir=dir.includes('/')?dir.slice(0,dir.lastIndexOf('/')):'';refresh()}}"
"function syncAll(){const boxes=[...document.querySelectorAll('#items input[type=checkbox]')];const all=document.getElementById('all');all.checked=boxes.length>0&&boxes.every(c=>c.checked);all.indeterminate=boxes.some(c=>c.checked)&&!all.checked}"
"function toggleAll(on){selected.clear();document.querySelectorAll('#items input[type=checkbox]').forEach(c=>{c.checked=on;if(on)selected.add(c.dataset.path)});syncAll()}"
"async function upload(){const files=[...document.getElementById('file').files];if(!files.length){msg('Seleccione PDF');return}for(const f of files){if(!f.name.toLowerCase().endsWith('.pdf')||f.size>33554432){msg('PDF inválido o mayor a 32 MB: '+f.name);return}try{msg('Subiendo '+f.name+'...');await request('upload',join(dir,f.name),'POST',f)}catch(e){msg('Error en '+f.name+': '+e.message);await refresh();return}}msg('PDF cargados correctamente');document.getElementById('file').value='';refresh()}"
"async function createFolder(){const name=prompt('Nombre de la carpeta nueva:');if(name===null)return;if(!name.trim()||name.includes('/')||name.includes('\\\\')||name.includes('..')){msg('Nombre inválido');return}try{await request('mkdir',join(dir,name.trim()),'POST');msg('Carpeta creada');refresh()}catch(e){msg(e.message)}}"
"async function moveSelected(){if(!selected.size){msg('Seleccione uno o varios PDF');return}const target=prompt('Carpeta destino relativa a Planos Equipamiento (vacío = raíz):','');if(target===null)return;let to=target.trim().replace(/^\\/+|\\/+$/g,'');if(to.includes('..')||to.includes('\\\\')){msg('Ruta inválida');return}if(!confirm('Mover '+selected.size+' PDF a '+(to||'raíz')+'?'))return;let errors=[];for(const p of selected){const name=p.split('/').pop();try{const r=await fetch(api+'move?from='+enc(p)+'&to='+enc(join(to,name)),{method:'POST'});if(!r.ok)throw Error(await r.text())}catch(e){errors.push(name+': '+e.message)}}msg(errors.length?errors.join(' | '):'Archivos movidos');refresh()}"
"async function deleteSelected(){if(!isAdmin)return;if(!selected.size){msg('Seleccione archivos o carpetas para eliminar');return}const paths=[...selected];const folders=paths.filter(p=>items.some(x=>x.dir&&join(dir,x.name)===p));const pdfs=paths.length-folders.length;const summary=(pdfs?pdfs+' PDF':'')+(pdfs&&folders.length?' y ':'')+(folders.length?folders.length+' carpeta(s) vacía(s)':'');if(!confirm('¿Eliminar definitivamente '+summary+'?\\nLas carpetas con contenido NO se eliminarán.'))return;let errors=[];for(const p of paths){try{await request('delete',p,'POST')}catch(e){errors.push(p+': '+e.message)}}await refresh();msg(errors.length?'No se pudieron eliminar todos: '+errors.join(' | '):'Elementos eliminados correctamente')}"
"async function init(){try{const r=await fetch(api+'role',{cache:'no-store'});if(!r.ok)throw Error('No autorizado');const role=await r.json();isAdmin=role.admin===true;document.getElementById('adminTools').hidden=!isAdmin;document.getElementById('selectTools').hidden=!isAdmin;document.getElementById('home').hidden=!isAdmin;await refresh()}catch(e){document.getElementById('items').textContent='Sesión no autorizada';msg(e.message)}}"
"init();"
"</script></body></html>"
;
static esp_err_t page_g(httpd_req_t *r){if(!access_ok(r))return ESP_OK;httpd_resp_set_type(r,"text/html; charset=utf-8");return httpd_resp_send(r,PAGE,HTTPD_RESP_USE_STRLEN);}
esp_err_t korvo_planos_register(httpd_handle_t server,bool (*authenticated)(httpd_req_t *),bool (*administrator)(httpd_req_t *)){
 s_auth=authenticated;
 s_admin=administrator;
 const httpd_uri_t routes[]={
  {.uri="/planos",.method=HTTP_GET,.handler=page_g},
  {.uri="/api/planos/tree",.method=HTTP_GET,.handler=tree_g},
  {.uri="/api/planos/role",.method=HTTP_GET,.handler=role_g},
  {.uri="/api/planos/download",.method=HTTP_GET,.handler=download_g},
  {.uri="/api/planos/upload",.method=HTTP_POST,.handler=upload_p},
  {.uri="/api/planos/mkdir",.method=HTTP_POST,.handler=mkdir_p},
  {.uri="/api/planos/delete",.method=HTTP_POST,.handler=delete_p},
  {.uri="/api/planos/move",.method=HTTP_POST,.handler=move_p},
 };
 for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]);i++){
  esp_err_t e=httpd_register_uri_handler(server,&routes[i]);if(e!=ESP_OK){ESP_LOGE(TAG,"No se pudo registrar ruta %u",(unsigned)i);return e;}
 }
 ESP_LOGI(TAG,"PLANOS01: repositorio PDF registrado en /planos");return ESP_OK;
}
