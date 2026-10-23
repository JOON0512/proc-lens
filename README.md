# proc-lens

Linux의 프로세스 정보와 메모리 수명을 공부하기 위한 읽기 전용 커널 모니터입니다. 커널 프로그래밍 학습을 바탕으로 2026년에 스냅샷 수집·동기화·해제 구조와 반복 실행 테스트를 새로 구현했습니다.

## 무엇을 볼 수 있나요?

`/proc/proc_lens/snapshot`에서 프로세스 리더의 PID·PPID·이름·CPU·우선순위·정책·CPU 시간·vruntime·weight·RSS·코드/데이터/힙 가상주소 구간을 TSV로 읽습니다. 시스템을 변경하는 명령은 제공하지 않습니다. 프로세스 가상주소를 포함하므로 root 및 `CAP_SYS_ADMIN` 권한이 필요합니다.

```sh
sudo apt install build-essential linux-headers-$(uname -r)
make
sudo insmod proc_lens.ko interval_ms=1000 max_tasks=4096
sudo cat /proc/proc_lens/snapshot
sudo rmmod proc_lens
```

실험용 VM에서 실행하세요. 테스트 환경은 Ubuntu의 **Linux 6.8.0-147-generic / x86-64**입니다. 다른 커널의 내부 API 호환성은 검증하지 않았습니다.

## 설계

- 수집은 `delayed_work`에서 수행합니다. 메모리 할당이나 mmap 잠금을 타이머의 원자적 문맥에서 실행하지 않습니다.
- RCU 구간에서는 task 참조만 확보합니다. 각 task는 `get_task_struct()` / `put_task_struct()`, 주소 공간은 `get_task_mm()` / `mmput()`으로 별도 관리합니다.
- 새 스냅샷을 완성한 뒤 짧은 mutex 구간에서 교체합니다. 스냅샷에는 복사한 값만 있으며 task/mm/페이지 테이블 포인터를 보관하지 않습니다.
- 파일을 열 때 스냅샷 참조를 하나 확보합니다. 부분 읽기와 seek, 수집 주기 사이에도 그 파일의 데이터는 유지됩니다. 파일 해제는 해당 참조만 내려놓습니다.
- 프로세스 수는 `max_tasks`로 제한하며 잘렸는지 헤더에 표시합니다. 할당 실패 시 마지막 정상 스냅샷을 유지하고 실패 횟수를 기록합니다.
- 제거 시 작업 종료를 기다리고 proc 콜백을 정리한 뒤 게시 중인 스냅샷을 해제합니다.

`memory_available=0`은 종료 중인 프로세스이거나 mmap 읽기 잠금을 즉시 얻지 못한 경우입니다. RSS와 스케줄러 필드는 실행 중 변할 수 있어 여러 필드가 같은 순간을 나타낸다고 보장하지 않습니다. 코드/데이터/힙 끝 주소는 **exclusive**이고, `initial_stack_pointer`는 전체 스택 매핑의 경계가 아닙니다. 물리주소 변환, 모든 스레드 나열, 특정 프로세스의 실시간 추적은 구현 범위에 없습니다. 정책 번호는 Linux `SCHED_*` 값이며, vruntime과 weight는 공정 스케줄링 정책에서 해석해야 합니다.

## 검증

2026-10-24, 네트워크·호스트 디스크 연결 없는 QEMU RAM 전용 VM에서 검증했습니다.

- 부분 읽기와 seek 후 스냅샷 일치, 새 파일에서 세대 증가
- 권한 없는 사용자 접근 거부
- 4개 읽기 스레드의 총 400회 조회와 200회 fork/exec/exit
- 파일을 열어 둔 상태에서 모듈 제거
- 잘못된 모듈 매개변수 거부 및 `max_tasks=1`에서 잘림 표시
- 50회 load/read/unload 및 proc 항목 정리
- 커널 로그의 BUG/Oops/WARNING/use-after-free/panic 검사 통과

이는 수행한 시나리오의 결과이며 모든 커널·부하에서의 무결성을 증명하지 않습니다. KASAN·lockdep·메모리 부족 강제 주입 검사는 아직 수행하지 않았습니다.

`tests/build-initramfs.sh KERNEL_RELEASE`는 해당 커널 이미지·헤더·static busybox·gcc·cpio가 설치된 일회용 Linux 빌드 환경에서 실행합니다. 만들어진 이미지는 다음과 같이 부팅할 수 있습니다.

```sh
qemu-system-x86_64 -m 768 -smp 2 -cpu max -accel tcg \
  -kernel vm/vmlinuz -initrd vm/initramfs.cpio.gz \
  -append 'console=ttyS0 rdinit=/init panic=-1 loglevel=4' \
  -nographic -no-reboot -nic none
```

성공 표시는 `PROC_LENS_TEST_SUCCESS`입니다. 테스트는 게스트에서만 모듈을 삽입합니다. Docker 호스트 커널에 모듈을 로드하지 않습니다.

참고: [Linux seq_file](https://docs.kernel.org/filesystems/seq_file.html), [workqueue](https://docs.kernel.org/core-api/workqueue.html). 소스는 GPL-2.0-only입니다.
