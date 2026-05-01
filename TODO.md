# 테스트 빚 (Test Debt) 추적

단위기능을 먼저 구현하고 검증은 나중으로 미룰 때, 잊지 않기 위해 여기에 적습니다.

## 사용 방법

- 새 단위기능을 `[unverified]` 상태로 커밋했으면 아래 "미검증" 섹션에 추가합니다.
- 형식: `- [ ] <한 줄 요약> — branch / commit <짧은 SHA> / 추가일`
- 실제 장비/실행으로 검증이 끝나면 `[x]` 로 체크하고 "검증 완료" 섹션으로 옮깁니다.
- 검증 중 문제가 발견되면 fixup 커밋(`git commit --fixup=<SHA>`)으로 수정하고, 항목 옆에 `→ fixup <SHA>` 를 덧붙입니다.

## 미검증 (Unverified)

### feature/xtouch-state-msg

- [ ] `/xtouch/state` 토픽이 페이더/터치/버튼 이벤트마다 정상 발행되는지 — 2026-05-01
  - 확인 항목: `ros2 topic echo /xtouch/state` 출력 형식, `fader_changed` 가 직전 발행 대비 정확히 갱신되는지
- [ ] Rec/Solo/Mute/Select 버튼 토글 (32개) — 2026-05-01
  - 확인 항목: 누름마다 `rec`/`solo`/`mute`/`select` 필드 반전, 뗌 무시
  - 노트 매핑: Rec `0..7`, Solo `8..15`, Mute `16..23`, Select `24..31`, MIDI 채널 0
- [ ] LED 미러링 — 2026-05-01
  - 확인 항목: 토글 ON → 해당 LED 점등, OFF → 소등
  - vel=127/0 으로 송신, 디바이스에서 실제 점등 확인
- [ ] 시작 시 LED 일제 OFF — 2026-05-01
  - 확인 항목: 노드 시작 직후 32개 LED 모두 꺼져 있는지 (디바이스가 이전 상태 기억하는 경우 대비)
- [ ] `target_ids` 파라미터(길이 8 정수 배열) 로드 및 `XTouchChannelState.target_id` 노출 — 2026-05-01
  - 확인 항목: 정상값 8개, 길이 불일치/음수/UINT32_MAX 초과 시 throw
- [ ] 페이더 motor hold 디바운스가 회귀 없는지 — 2026-05-01
  - 회귀 시나리오: 페이더 조작 off 시 0으로 돌아가지 않아야 함 (커밋 5f21820 의 의도 유지)
- [ ] Encoder rotate 카운터 + LED Ring — 2026-05-01
  - CC 80..87 수신, value 1..63 = CW(+1), 65..127 = CCW(-1)
  - 카운터 0..11 saturate (12 클릭 이상 돌려도 11 유지, 0 미만 0 유지)
  - 회전마다 LED ring (CC 48..55) 가 한 칸씩 점등/소등 (왼쪽부터 채움)
- [ ] Encoder push 내부 토글 — 2026-05-01
  - Note 32..39 누름마다 `encoder_push_state_` 반전, 뗌 무시
  - 외부 노출 없음 (로그 `encoder_push[N] = on/off` 만 확인)
- [ ] 시작 시 LED Ring 일제 0 송신 — 2026-05-01
  - 노드 시작 직후 8개 ring 모두 소등 상태인지 (디바이스 잔존 표시 클리어)

## 검증 완료 (Done)

- [x] 새 메시지(`XTouchState`, `XTouchChannelState`) `colcon build` 통과 및 `ros2 interface show xtouch_midi/msg/XTouchState` 확인 — 2026-05-01
  - rec/solo/mute/select bool 필드 노출 확인됨
