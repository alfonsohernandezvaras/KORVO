#include "face_detect.h"

#include "human_face_detect.hpp"
#include "dl_image_define.hpp"

#include <new>

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
