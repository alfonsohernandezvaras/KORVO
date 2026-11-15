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

#define ROOT BSP_SD_MOUNT_POINT "/korvo/planos equipamiento"
#define MAX_PATH 384
#define MAX_UPLOAD (32U*1024U*1024U)
static bool (*s_auth)(httpd_req_t *) = NULL;
static bool s_busy = false;
static const char *TAG="korvo_planos";

static esp_err_t reply(httpd_req_t *r,const char *status,const char *msg){
 httpd_resp_set_status(r,status);httpd_resp_set_type(r,"text/plain; charset=utf-8");return httpd_resp_sendstr(r,msg);
}
static bool access_ok(httpd_req_t *r){if(s_auth && s_auth(r))return true;reply(r,"403 Forbidden","Acceso no autorizado");return false;}
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
 if(!access_ok(r))return ESP_OK;
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
 if(!access_ok(r))return ESP_OK;
 if(s_busy)return reply(r,"503 Service Unavailable","Transferencia en curso");
 char rel[320],path[MAX_PATH];if(!query_path(r,rel,sizeof(rel))||!rel[0]||!safe_path(rel,path,sizeof(path))||pdf_name(rel))return reply(r,"400 Bad Request","Nombre invalido");
 char parent[MAX_PATH];strcpy(parent,path);char *slash=strrchr(parent,'/');if(!slash)return reply(r,"400 Bad Request","Ruta invalida");*slash=0;
 struct stat st;if(stat(parent,&st)!=0||!S_ISDIR(st.st_mode))return reply(r,"404 Not Found","Carpeta padre no existe");
 if(mkdir(path,0775)!=0)return reply(r,errno==EEXIST?"409 Conflict":"500 Internal Server Error","No se pudo crear carpeta");
 return reply(r,"201 Created","Carpeta creada");
}
static const char PAGE[] =
"<!doctype html><html lang='es'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Planos Equipamiento</title>"
"<style>body{font:14px Arial;background:#10151b;color:#e9eef4;margin:0}header{background:#171f28;padding:14px}main{padding:18px;max-width:1000px}button,input{padding:8px;background:#202c37;color:white;border:1px solid #445363;border-radius:5px}button{cursor:pointer}.folder{color:#8ecbff}.file{color:#d6e6f8}.item{padding:8px;border-bottom:1px solid #34404d;cursor:pointer}.item:hover{background:#253442}.child{margin-left:22px}section{background:#18212b;padding:15px;margin:12px 0;border-radius:8px}a{color:#8ecbff}small{color:#9eabb8}</style></head>"
"<body><header><a href='/'>Estado</a> &nbsp; / &nbsp; <b>Planos Equipamiento</b></header><main><h2>Repositorio documental SD</h2><small>Seleccione un PDF para descargarlo directamente al PC.</small><section id='tree'>Cargando...</section>"
"<section><h3>Administrar documentos</h3><small>Ruta relativa dentro de Planos Equipamiento (ejemplo: Electricos/Unilineal.pdf)</small><p><input id='path' style='width:70%' placeholder='Carpeta/Archivo.pdf'></p><input id='file' type='file' accept='.pdf,application/pdf'><button onclick='upload()'>Subir PDF</button><p><input id='folder' placeholder='Nombre de carpeta'><button onclick='makeFolder()'>Crear carpeta</button></p><small id='msg'></small></section></main>"
"<script>const api='/api/planos/';function url(p){return encodeURIComponent(p).replace(/%2F/g,'/')}function msg(s){document.getElementById('msg').textContent=s}"
"async function load(dir,host){let r=await fetch(api+'tree?path='+url(dir));if(!r.ok){host.textContent='Error al leer SD';return}let data=await r.json();host.textContent='';if(!data.length){host.textContent='(Carpeta vacia)';return}data.sort((a,b)=>Number(b.dir)-Number(a.dir)||a.name.localeCompare(b.name));for(let x of data){let p=dir?dir+'/'+x.name:x.name;let row=document.createElement('div');row.className='item '+(x.dir?'folder':'file');row.textContent=(x.dir?'📁 ':'📄 ')+x.name+(x.dir?'':' ('+Math.round(x.size/1024)+' KB)');host.appendChild(row);if(x.dir){let child=document.createElement('div');child.className='child';host.appendChild(child);let open=false;row.onclick=()=>{open=!open;if(open)load(p,child);else child.textContent=''}}else row.onclick=()=>{window.location.href=api+'download?path='+url(p)}}}"
"async function upload(){let f=document.getElementById('file').files[0],p=document.getElementById('path').value.trim();if(!f||!p.toLowerCase().endsWith('.pdf')){msg('Seleccione PDF y ruta .pdf');return}if(f.size>33554432){msg('Maximo 32 MB');return}msg('Subiendo...');try{let r=await fetch(api+'upload?path='+url(p),{method:'POST',headers:{'Content-Type':'application/pdf'},body:f});msg(await r.text());if(r.ok)load('',document.getElementById('tree'))}catch(e){msg('Error de red')}}"
"async function makeFolder(){let p=document.getElementById('folder').value.trim();if(!p)return;try{let r=await fetch(api+'mkdir?path='+url(p),{method:'POST'});msg(await r.text());if(r.ok)load('',document.getElementById('tree'))}catch(e){msg('Error de red')}}load('',document.getElementById('tree'));</script></body></html>";
static esp_err_t page_g(httpd_req_t *r){if(!access_ok(r))return ESP_OK;httpd_resp_set_type(r,"text/html; charset=utf-8");return httpd_resp_send(r,PAGE,HTTPD_RESP_USE_STRLEN);}
esp_err_t korvo_planos_register(httpd_handle_t server,bool (*authenticated)(httpd_req_t *)){
 s_auth=authenticated;
 const httpd_uri_t routes[]={
  {.uri="/planos",.method=HTTP_GET,.handler=page_g},
  {.uri="/api/planos/tree",.method=HTTP_GET,.handler=tree_g},
  {.uri="/api/planos/download",.method=HTTP_GET,.handler=download_g},
  {.uri="/api/planos/upload",.method=HTTP_POST,.handler=upload_p},
  {.uri="/api/planos/mkdir",.method=HTTP_POST,.handler=mkdir_p},
 };
 for(size_t i=0;i<sizeof(routes)/sizeof(routes[0]);i++){
  esp_err_t e=httpd_register_uri_handler(server,&routes[i]);if(e!=ESP_OK){ESP_LOGE(TAG,"No se pudo registrar ruta %u",(unsigned)i);return e;}
 }
 ESP_LOGI(TAG,"PLANOS01: repositorio PDF registrado en /planos");return ESP_OK;
}
