# TEA Hybrid Chain / Periodic Reset 계획

**최종 갱신**: 2026-04-27
**상태**: 아직 미구현, Work F 안정화 이후 진행

---

## 1. 목적

현재 TEA Fetch는 Dependency Chain Cache entry를 H2P PC로 직접 조회한다. 이 entry는 BW Walk 시점에 만들어진 단일 chain에 가깝기 때문에, control-flow path가 달라질 때 다중 경로 dependency 정보를 충분히 보존하지 못할 수 있다.

Hybrid Chain 작업의 목적은 Block Cache에 누적된 basic-block bitmask 정보를 사용해 Dep Chain Cache entry를 더 robust한 합집합 chain으로 갱신하는 것이다.

---

## 2. 현재 상태

- Block Cache는 BW Walk 과정에서 채워진다.
- TEA Fetch는 Block Cache를 직접 조회하지 않는다.
- Dep Chain Cache는 H2P PC 기준으로 TEA Fetch의 입력이 된다.
- `periodically_reset_caches()` 연결은 아직 현재 구현의 완료 항목으로 보지 않는다.

---

## 3. 계획

### Dep Chain Cache metadata 확장

Dep Chain Cache entry가 chain을 구성한 basic block PC 목록을 보관하게 한다. 이후 같은 H2P PC에 대한 BW Walk가 들어오면 기존 block 목록과 새 block 목록을 합쳐 chain을 재구성한다.

### Block Cache OR mask 활용

각 basic block의 dependency bitmask를 OR 누적하고, H2P chain을 재구성할 때 block별 dependent op segment를 합친다. TEA Fetch interface는 유지한다.

### Periodic reset

오래된 path 정보가 과도하게 누적되지 않도록 일정 retired main op 간격으로 Block Cache와 관련 dep-chain metadata를 reset한다.

---

## 4. Work F와의 관계

Work F는 여러 H2P chain을 동시에 실행하는 구조다. Hybrid Chain은 각 H2P chain의 입력 품질을 높이는 작업이다. 따라서 구현 순서는 다음이 맞다.

1. Work F 안정화.
2. Work F 통계로 chain miss/staleness 또는 early flush 기회 부족을 확인.
3. Hybrid Chain 적용.
4. 같은 simpoint로 성능/정확성 비교.

---

## 5. 검증 포인트

- Dep Chain Cache hit rate 변화.
- Chain length 분포 변화.
- `TEA_TRIGGERS`와 `TEA_EARLY_FLUSHES` 변화.
- TEA op 수 증가에 따른 RS/preg/store-buffer pressure.
- IPC TEA on/off 비교.
