# MP1 Sender 구현 계획서

## 0. 현재 상태 요약

이 폴더에는 과제에서 제공한 실행/라이브러리/입력 파일이 있다.

- `mp1.pdf`: 과제 명세
- `netsim`: 채널 및 receiver 시뮬레이터 바이너리
- `netsim.h`: `send_frame()` API와 반환값 정의
- `netsim_lib.cc`: `send_frame()` 구현. sender의 `stdin/stdout`을 netsim과 통신용 pipe로 사용
- `sherlock_holmes.txt`, `harry_potter.txt`, `cat_bgm.mp3`: validation 입력 파일

현재 제출용 sender 소스 파일은 없다. 새로 만들 파일은 `sender_<학번>.cc` 형식이어야 하며, 예를 들어 이 계정의 학번을 따르면 `sender_20221599.cc`가 자연스럽다.

중요: `netsim.h`, `netsim_lib.cc`, `netsim`, 입력 파일들은 제공 파일이므로 수정하지 않는다.

## 1. 명세 핵심 요구사항 정리

sender는 netsim이 다음 형태로 실행한다.

```bash
./sender input_file
```

sender는 입력 파일을 읽고, 데이터를 payload 단위로 나누어 다음 프레임 구조로 전송한다.

```text
[size: 2 bytes big-endian] [payload: 1~65535 bytes] [CRC: 4 bytes big-endian]
```

CRC-32는 `size` 필드와 `payload`를 합친 `2 + P` bytes에 대해 계산한다. 계산 결과 32비트 값은 big-endian으로 프레임 끝에 붙인다.

전송은 `send_frame(frame, frame_size)`만 사용한다.

- `NETSIM_ACK`: 해당 payload가 정상 수신되었으므로 다음 데이터로 진행
- `NETSIM_NAK`: 해당 데이터가 손상되었으므로 다시 전송
- `NETSIM_ERROR`: 통신 오류. 발생하면 fatal로 보고 종료해도 됨

모든 데이터가 ACK된 뒤에는 `return 0`으로 종료한다. netsim은 sender 종료를 보고 출력 파일과 통계를 만든다.

## 2. 구현 목표

정확성 목표:

- 입력 파일의 모든 byte를 빠짐없이, 순서대로 ACK 받게 한다.
- frame format, big-endian size/CRC, CRC 계산 범위를 정확히 지킨다.
- binary 파일도 처리해야 하므로 `std::string`의 C-string 가정이나 텍스트 모드 처리를 피한다.

효율성 목표:

- cost는 `bytes_total + K * frames_total`, 기본 `K = 250`.
- payload가 너무 작으면 frame 수가 많아져 round-trip 비용이 커진다.
- payload가 너무 크면 BER이 높을 때 NAK 확률이 커져 재전송 낭비가 커진다.
- BER은 sender에게 직접 전달되지 않으므로 ACK/NAK 패턴을 보고 payload 크기를 적응적으로 조절한다.

## 3. Step-by-Step 구현 계획

### Step 1. 새 sender 파일 생성

새 파일:

```text
CN/mp1/sender_20221599.cc
```

기본 include 후보:

```cpp
#include "netsim.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>
```

`main(int argc, char** argv)`에서 인자 개수가 2인지 확인하고, 실패 시 `stderr`에 메시지를 남긴 뒤 non-zero로 종료한다.

### Step 2. 입력 파일 전체 읽기

binary 모드로 파일을 연다.

```cpp
std::ifstream in(argv[1], std::ios::binary);
```

파일 전체를 `std::vector<uint8_t>`에 저장한다. 텍스트와 MP3 모두 byte 단위로 동일하게 처리해야 한다.

빈 파일 가능성은 명세에 명확히 나오지 않았지만, payload 크기 0인 프레임은 금지되어 있다. 빈 파일이면 아무 프레임도 보내지 않고 정상 종료하는 처리가 가장 안전하다.

### Step 3. CRC-32 구현

명세의 generator polynomial은 Ethernet CRC-32 다항식이다.

```text
x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 + x^10
+ x^8 + x^7 + x^5 + x^4 + x^2 + x + 1
```

명세는 "기본 modulo-2 division 방식"을 요구한다. 구현 선택지는 두 가지다.

- 우선 구현: bitwise MSB-first modulo-2 division으로 명세에 맞춰 직접 구현
- 성능 개선: 같은 결과를 내는 table 기반 CRC-32를 직접 생성해서 사용

검증이 쉬운 방향은 bitwise 구현이다. 파일 크기가 수 MB라 validation에서도 충분히 감당 가능하다. 다만 NAK으로 재전송이 많으면 CRC 재계산이 반복되므로, 성능이 부족하면 table 기반으로 바꾼다.

주의할 점:

- CRC 입력은 `[size 2B][payload P bytes]`
- CRC 결과는 frame 끝에 big-endian으로 저장
- CRC 계산 함수의 입력은 `uint8_t*`와 길이 또는 `std::vector<uint8_t>` 구간으로 설계

### Step 4. 프레임 생성 함수 작성

권장 함수 형태:

```cpp
std::vector<uint8_t> make_frame(
    const std::vector<uint8_t>& data,
    size_t offset,
    size_t payload_size
);
```

함수 내부 작업:

1. `payload_size`가 `1..65535` 범위인지 확인
2. frame 크기를 `2 + payload_size + 4`로 잡기
3. size field를 big-endian으로 저장
4. payload 복사
5. 앞의 `2 + payload_size` bytes에 CRC-32 계산
6. CRC를 big-endian 4 bytes로 저장

### Step 5. 기본 Stop-and-Wait 루프 작성

핵심 상태:

- `offset`: 아직 ACK 받지 못한 다음 입력 byte 위치
- `payload_size`: 다음 시도에 사용할 payload 크기
- `remaining = data.size() - offset`

반복 구조:

1. `P = min(payload_size, remaining, 65535)`
2. frame 생성
3. `send_frame(frame.data(), frame.size())`
4. ACK이면 `offset += P`
5. NAK이면 `offset`은 그대로 두고 같은 데이터 구간을 다시 시도
6. ERROR면 오류 메시지 후 종료

가장 단순한 버전은 항상 같은 payload 크기를 사용하는 것이다. 정확성 확보를 위해 먼저 이 버전을 완성한다.

### Step 6. Payload 크기 전략 1차 적용

처음부터 과도하게 복잡한 전략을 넣기보다, 정확한 sender를 만든 뒤 적응형으로 개선한다.

초기 payload 후보:

- 좋은 채널에서 효율적인 큰 값: `32000` 또는 `48000`
- 중간 균형 값: `16000`

BER이 `1e-6`이면 큰 payload도 성공 확률이 높다. BER이 `1e-3`이면 큰 payload는 거의 계속 NAK이 나므로 빠르게 줄여야 한다.

1차 적응 규칙 예시:

- ACK가 연속으로 몇 번 나오면 payload를 점진적으로 증가
- NAK이 나오면 payload를 절반으로 감소
- 최소 payload는 `1` 또는 `32`
- 최대 payload는 `65535`

예시 정책:

```text
initial P = 8192
on ACK:
  consecutive_ack += 1
  if consecutive_ack >= 8: P = min(P * 2, 65535), reset consecutive_ack
on NAK:
  consecutive_ack = 0
  P = max(P / 2, 1)
```

이 정책은 BER을 모르는 상황에서 안전하게 수렴한다. 단, P가 1까지 내려가면 frame overhead가 커지므로 validation 결과를 보고 최소값과 증가 속도를 조정한다.

### Step 7. NAK 후 분할 전략 개선

명세상 NAK을 받은 큰 프레임은 같은 크기로 재전송할 필요가 없다. 같은 offset에서 더 작은 P로 재시도할 수 있다.

개선 방향:

- NAK 즉시 절반으로 줄인다.
- 작은 P에서 ACK가 누적되면 서서히 키운다.
- 너무 급격히 키우면 다시 NAK이 많아지므로 곱셈 증가보다 덧셈 증가가 안정적일 수 있다.

비교할 전략:

```text
전략 A: ACK 8회마다 P *= 2, NAK마다 P /= 2
전략 B: ACK마다 P += 1024, NAK마다 P /= 2
전략 C: ACK 연속 구간에서는 P *= 1.25 정도로 완만히 증가
```

C++에서는 floating 계산 없이 `P = P + max<size_t>(1, P / 4)`처럼 구현할 수 있다.

### Step 8. Validation 실행

컴파일:

```bash
g++ -O2 -o sender_20221599 sender_20221599.cc netsim_lib.cc
```

실행은 `CN/mp1` 디렉토리에서 한다.

```bash
./netsim ./sender_20221599 --input sherlock_holmes.txt --output out.rx --ber 1e-6 --seed 1001 --max_bytes 386832300
diff sherlock_holmes.txt out.rx

./netsim ./sender_20221599 --input cat_bgm.mp3 --output out.rx --ber 1e-5 --seed 2002 --max_bytes 316224000
diff cat_bgm.mp3 out.rx

./netsim ./sender_20221599 --input cat_bgm.mp3 --output out.rx --ber 1e-4 --seed 3003 --max_bytes 316224000
diff cat_bgm.mp3 out.rx

./netsim ./sender_20221599 --input harry_potter.txt --output out.rx --ber 1e-3 --seed 4004 --max_bytes 44276800
diff harry_potter.txt out.rx
```

각 실행에서 확인할 항목:

- `status: SUCCESS`
- `diff` 결과가 없음
- `MAX_BYTES_EXCEEDED`가 발생하지 않음
- `cost`, `bytes_total`, `frames_total`, `frames_naked` 기록

### Step 9. 전략 튜닝

validation별 예상 경향:

- Validation 1, BER `1e-6`: 큰 payload가 유리
- Validation 2, BER `1e-5`: 중대형 payload가 유리
- Validation 3, BER `1e-4`: 중간 payload가 유리
- Validation 4, BER `1e-3`: 작은 payload가 유리

튜닝 순서:

1. 정확성 실패를 먼저 제거한다.
2. `frames_naked`가 지나치게 많으면 NAK 시 감소를 더 강하게 한다.
3. `frames_total`이 지나치게 많으면 ACK 시 증가를 더 빠르게 한다.
4. 고 BER에서 `max_bytes`에 가까워지면 초기 P를 낮추거나 NAK 후 최소 P 도달을 빠르게 한다.

### Step 10. 제출 전 체크리스트

- 파일명: `sender_20221599.cc`
- 제공 파일 수정 없음
- 외부 라이브러리 사용 없음
- `stdout`/`stdin` 직접 사용 없음
- debug 출력은 필요하면 `stderr`만 사용
- 컴파일 명령이 명세와 동일하게 성공
- 텍스트와 MP3 모두 `diff` 통과
- 빈 파일 또는 아주 작은 파일도 처리 가능
- payload size 0 프레임을 절대 보내지 않음
- CRC 계산 범위가 size + payload인지 재확인
- size와 CRC 모두 big-endian인지 재확인

## 4. 구현 중 자주 틀릴 수 있는 지점

- CRC를 payload에 대해서만 계산하는 실수: 반드시 size 2 bytes도 포함
- little-endian으로 size 또는 CRC를 넣는 실수
- NAK 후 offset을 증가시키는 실수
- `send_frame()`에 payload 크기만 넘기고 전체 frame 크기를 넘기지 않는 실수
- binary 파일을 텍스트 모드 또는 문자열 종료 문자 기준으로 읽는 실수
- `stdout`에 로그를 출력해 netsim pipe 프로토콜을 깨는 실수
- 마지막 조각에서 remaining보다 큰 payload를 복사하는 실수

## 5. 권장 진행 순서

1. `sender_20221599.cc` 뼈대 작성
2. 파일 binary read 구현
3. CRC-32 함수 구현
4. frame 생성 함수 구현
5. 고정 payload Stop-and-Wait 구현
6. 작은 임시 파일로 BER 0 또는 매우 낮은 BER 테스트
7. validation 1~4 정확성 확인
8. 적응형 payload 전략 추가
9. validation cost 비교표 작성
10. 최종 코드 정리 후 제출

## 6. Cost 기록 표 템플릿

튜닝할 때 아래 표를 채워 두면 전략 비교가 쉽다.

| Strategy | Validation | status | bytes_total | frames_total | frames_naked | cost |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| fixed 8192 | V1 |  |  |  |  |  |
| fixed 8192 | V2 |  |  |  |  |  |
| fixed 8192 | V3 |  |  |  |  |  |
| fixed 8192 | V4 |  |  |  |  |  |
| adaptive A | V1 |  |  |  |  |  |
| adaptive A | V2 |  |  |  |  |  |
| adaptive A | V3 |  |  |  |  |  |
| adaptive A | V4 |  |  |  |  |  |
