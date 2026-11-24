#include "face_detect.h"

#include "human_face_detect.hpp"
#include "dl_image_define.hpp"

#include <new>
/* FR02A: diagnostic only, called from existing face task. */
#include "human_face_recognition.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <cmath>
#include <vector>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static std::atomic<bool> fr02a_enabled{false};
extern "C" void fr02a_enable(void) { fr02a_enabled.store(true); ESP_LOGI("FR02A", "R1 BOOT GATE ENABLED"); }
#include <list>
static void fr04_mem(const char *stage) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI("FR02A", "R4 %s internal=%u largest=%u min=%u PSRAM=%u ps_largest=%u", stage,
        (unsigned)heap_caps_get_free_size(caps), (unsigned)heap_caps_get_largest_free_block(caps),
        (unsigned)heap_caps_get_minimum_free_size(caps), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
static bool fr02a_r3_points(const dl::image::img_t &img, const dl::detect::result_t &face, std::vector<int> &points)
{
#if CONFIG_FLASH_HUMAN_FACE_DETECT_MSRMNP_S8_V1 && CONFIG_HUMAN_FACE_DETECT_MODEL_IN_FLASH_RODATA
    human_face_detect::MNP *mnp = nullptr;
    static int64_t last = 0;
    if (!fr02a_enabled.load() || xPortGetCoreID()!=1) return false;
    const int64_t now=esp_timer_get_time();
    if(now-last<3000000) return false;
    last=now;
    if(face.box.size()!=4 || face.box[0]<0 || face.box[1]<0 || face.box[2]>=img.width || face.box[3]>=img.height || face.box[2]<=face.box[0] || face.box[3]<=face.box[1]) return false;
    if(!mnp) {
        ESP_LOGI("FR02A", "R4 MNP LOAD START internal=%u PSRAM=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        mnp=new(std::nothrow) human_face_detect::MNP("human_face_detect_mnp_s8_v1.espdl",0.5f,0.5f);
        if(!mnp) return false;
        ESP_LOGI("FR02A", "R4 MNP READY load_ms=%.2f",(esp_timer_get_time()-now)/1000.0);
    }
    std::list<dl::detect::result_t> candidates;
    candidates.push_back(face);
    const int64_t start=esp_timer_get_time();
    auto &found=mnp->run(img,candidates);
    const unsigned count=found.size();
    const unsigned n=count==1 ? found.front().keypoint.size():0;
    bool valid=n==10;
    if(valid) for(int i=0;i<10;i++) {int p=found.front().keypoint[i]; if(p<0 || p>=((i%2)?img.height:img.width)) valid=false;}
    ESP_LOGI("FR02A", "R4 MNP faces=%u landmarks=%u valid=%d ms=%.2f internal=%u PSRAM=%u",count,n,(int)valid,(esp_timer_get_time()-start)/1000.0,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if(valid) points=found.front().keypoint;
    delete mnp;
    fr04_mem("AFTER MNP FREE");
    return valid;
#else
    (void)img; (void)face; (void)points; return false;
#endif
}

static void fr02a_probe(const dl::image::img_t &img, const std::vector<int> &points)
{
    (void)img;
    if (!fr02a_enabled.load() || points.size()!=10) return;
    fr04_mem("BEFORE MFN");
    ESP_LOGW("FR02A", "R4 MFN HOLD: allocator budget not verified; diagnostic only");
}




static HumanFaceDetect *s_detector = nullptr;

extern "C" bool face_detect_init(void)
{
    if (s_detector != nullptr) {
        return true;
    }

    s_detector = new (std::nothrow)
        HumanFaceDetect(HumanFaceDetect::ESPDET_PICO_224_224_FACE);

    return s_detector != nullptr;
}

extern "C" bool face_detect_run(const uint8_t *rgb565be,
                                uint16_t width,
                                uint16_t height,
                                face_detection_t *result)
{
    if (result == nullptr) {
        return false;
    }

    result->detected = false;
    result->eye_roi_valid = false;
    result->fr01_points_valid = false;
    result->score = 0.0f;
    result->x1 = result->y1 = result->x2 = result->y2 = 0;

    if (s_detector == nullptr || rgb565be == nullptr) {
        return false;
    }

    dl::image::img_t img = {};
    img.data = const_cast<uint8_t *>(rgb565be);
    img.width = width;
    img.height = height;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;

    auto &results = s_detector->run(img);

    static int64_t diag_last = 0;
    const int64_t diag_now = esp_timer_get_time();
    const bool diag = diag_now - diag_last >= 1000000;
    if (diag) {
        diag_last = diag_now;
        ESP_LOGI("FR02A", "R1 gate=%d core=%d faces=%u", (int)fr02a_enabled.load(), (int)xPortGetCoreID(), (unsigned)results.size());
    }
    if (results.empty()) {
        return true;
    }

    const auto &face = results.front();

    if (face.box.size() < 4) {
        return true;
    }

    result->detected = true;
    result->score = face.score;
    if (face.keypoint.size() == 10) {
        bool valid = true;
        for (int i=0; i<10; ++i) {
            const int p = face.keypoint[i];
            result->fr01_points[i] = p;
            if (p < 0 || p >= ((i % 2) ? height : width)) valid = false;
        }
        result->fr01_points_valid = valid;
    }
    if (diag) {
        ESP_LOGI("FR02A", "R1 landmarks=%u valid=%d frame=%ux%u reason=%s", (unsigned)face.keypoint.size(), (int)result->fr01_points_valid, (unsigned)width, (unsigned)height, !fr02a_enabled.load() ? "GATE_OFF" : results.size()!=1 ? "MULTIPLE_FACES" : face.keypoint.size()!=10 ? "LANDMARK_COUNT" : !result->fr01_points_valid ? "LANDMARK_RANGE" : "ELIGIBLE");
    }
    if (results.size() == 1 && fr02a_enabled.load()) {
        std::vector<int> biometric_points;
        if (fr02a_r3_points(img, face, biometric_points)) fr02a_probe(img, biometric_points);
    }
    result->x1 = face.box[0];
    result->y1 = face.box[1];
    result->x2 = face.box[2];
    result->y2 = face.box[3];
    /* IA03_EYE_ROI: ROI aproximadas derivadas de la caja facial.
     * No mide ojos abiertos/cerrados. */
    const int fw = result->x2 - result->x1;
    const int fh = result->y2 - result->y1;
    if (fw >= 48 && fh >= 48) {
        const int lx1 = result->x1 + fw * 17 / 100;
        const int lx2 = result->x1 + fw * 46 / 100;
        const int rx1 = result->x1 + fw * 54 / 100;
        const int rx2 = result->x1 + fw * 83 / 100;
        const int y1 = result->y1 + fh * 25 / 100;
        const int y2 = result->y1 + fh * 48 / 100;
        if (lx1 >= 0 && lx2 < (int)width && rx1 >= 0 &&
            rx2 < (int)width && y1 >= 0 && y2 < (int)height &&
            lx1 < lx2 && rx1 < rx2 && y1 < y2) {
            result->eye_left_x1=lx1; result->eye_left_y1=y1;
            result->eye_left_x2=lx2; result->eye_left_y2=y2;
            result->eye_right_x1=rx1; result->eye_right_y1=y1;
            result->eye_right_x2=rx2; result->eye_right_y2=y2;
            result->eye_roi_valid=true;
        }
    }

    return true;
}
