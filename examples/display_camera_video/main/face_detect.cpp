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
    result->x1 = face.box[0];
    result->y1 = face.box[1];
    result->x2 = face.box[2];
    result->y2 = face.box[3];

    return true;
}
