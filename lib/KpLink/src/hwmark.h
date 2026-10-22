/**
 * @file      hwmark.h
 * @brief     OTA 이미지의 하드웨어 세대 마커 — "다른 세대 펌웨어가 OTA 로 들어오는 것" 차단
 *
 * v1(T-A7670E 단일 보드)과 v2(메인 + 서브 2보드)는 같은 ESP32 를 쓰지만 핀 용도가 다르다.
 * v2 메인은 GPIO13/34 를 서브 링크로 쓰고 차키를 서브로 넘긴다 — 이 이미지가 v1 단말에
 * 들어가면 차키와 콘솔이 죽고, 원격 복구 경로도 장담할 수 없다.
 *
 * 그래서 이미지 자체에 `KPHW:<모델>` 문자열을 심고(kMarker), 단말은 내려받는 바이트 스트림을
 * 훑어 **다른 모델의 마커**가 보이면 Update.end() 전에 중단한다. 서버가 ota_start 의 hw 필드를
 * 잘못 보내거나 아예 안 보내도 막힌다 — 근거가 서버 라벨이 아니라 이미지 내용이기 때문이다.
 *
 * ⚠️ 마커가 **없는** 이미지는 통과시킨다. 0.5.0 이하 v1 이미지에는 마커가 없으므로, 막으면
 *    v1 롤백이 불가능해진다. v2 이미지는 처음부터 마커를 갖고 나가므로 이 예외로 새지 않는다.
 * ⚠️ 접두어 "KPHW:" 를 이 파일 밖에서 **별도 문자열 리터럴로 쓰지 말 것.** 이미지 안에
 *    "KPHW:" 다음 바로 NUL 인 조각이 생기면 빈 모델명으로 읽혀 자기 이미지를 거부한다.
 *    스캐너는 접두어를 kMarker 배열에서 직접 읽는다(리터럴을 따로 두지 않는다).
 *
 * Arduino 의존이 없어 호스트에서 단위 테스트한다(test/test_hwmark).
 *
 * 위치: lib/KpLink — 메인(HW_MODEL "v2")과 서브("v2-sub")가 같은 스캐너를 쓴다.
 * include 하기 전에 각자의 config.h 로 HW_MODEL 을 정의해야 한다.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef HW_MODEL
#error "HW_MODEL 이 정의돼야 한다 (config.h)"
#endif

namespace HwMark {

static const size_t kPrefixLen = 5;   // "KPHW:"
static const size_t kModelMax  = 15;  // 모델명 최대 길이(넘으면 마커로 보지 않는다)

// 이 이미지의 마커. used — 링커가 지우지 않게 한다(스캐너가 참조도 하지만 이중 보장).
__attribute__((used)) static const char kMarker[] = "KPHW:" HW_MODEL;

// 스트림 스캐너. feed() 를 청크마다 부르고, 끝에서 foreign()/matched() 를 본다.
// 청크 경계에 걸친 마커도 잡는다(상태가 청크를 넘어 이어진다).
class Scanner {
public:
    void feed(const uint8_t *p, size_t n)
    {
        for (size_t i = 0; i < n; i++) step((char)p[i]);
    }
    bool matched() const { return matched_; }   // 자기 모델 마커를 봤다
    bool foreign() const { return foreign_; }   // 다른 모델 마커를 봤다
    const char *foreignModel() const { return foreignModel_; }

private:
    void step(char c)
    {
        if (idx_ < kPrefixLen) {
            if (c == kMarker[idx_]) { idx_++; mlen_ = 0; return; }
            idx_ = (c == kMarker[0]) ? 1 : 0;
            return;
        }
        // 접두어 뒤 — 모델명을 NUL 까지 모은다.
        if (c == '\0') {
            if (mlen_ > 0) finish();
            idx_ = 0;
            return;
        }
        if (mlen_ >= kModelMax || c < 0x21 || c > 0x7E) {   // 인쇄 가능 문자 아님 → 마커 아님
            idx_ = (c == kMarker[0]) ? 1 : 0;
            return;
        }
        model_[mlen_++] = c;
    }

    void finish()
    {
        model_[mlen_] = '\0';
        if (strcmp(model_, kMarker + kPrefixLen) == 0) {
            matched_ = true;
        } else if (!foreign_) {
            foreign_ = true;
            memcpy(foreignModel_, model_, mlen_ + 1);
        }
    }

    size_t idx_  = 0;
    size_t mlen_ = 0;
    char   model_[kModelMax + 1] = {0};
    char   foreignModel_[kModelMax + 1] = {0};
    bool   matched_ = false;
    bool   foreign_ = false;
};

} // namespace HwMark
