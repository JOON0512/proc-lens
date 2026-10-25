# Development notes

## Snapshot fields

각 행은 프로세스 리더 하나입니다. 모든 스레드나 물리주소 변환은 포함하지 않습니다.

- `memory_available=0`: 종료 중이거나 mmap 읽기 잠금을 얻지 못한 프로세스
- 코드·데이터·힙의 끝 주소: exclusive
- `initial_stack_pointer`: 초기 스택 포인터이며 전체 스택 매핑의 경계가 아님
- 정책 번호: Linux `SCHED_*` 값
- vruntime과 weight: 공정 스케줄링 정책에서 해석

RSS와 스케줄러 필드는 수집 중에도 바뀔 수 있습니다. 여러 필드가 정확히 같은 순간을 나타내지는 않습니다. 할당 실패 시 마지막 정상 스냅샷을 유지하고 실패 횟수를 기록합니다.

## QEMU tests

Linux 빌드 환경에 커널 이미지·헤더, static busybox, gcc, cpio가 필요합니다.

```sh
tests/build-initramfs.sh KERNEL_RELEASE
qemu-system-x86_64 -m 768 -smp 2 -cpu max -accel tcg \
  -kernel vm/vmlinuz -initrd vm/initramfs.cpio.gz \
  -append 'console=ttyS0 rdinit=/init panic=-1 loglevel=4' \
  -nographic -no-reboot -nic none
```

성공 시 `PROC_LENS_TEST_SUCCESS`를 출력합니다. 모듈은 QEMU 게스트에 적재합니다.

검사 항목은 부분 읽기와 seek, 세대 변경, 접근 권한, 동시 조회와 프로세스 생성·종료, 열린 FD를 유지한 모듈 제거, 매개변수와 잘림 처리, 50회 load/read/unload입니다.

테스트 환경은 Ubuntu Linux 6.8.0-147-generic / x86-64입니다. KASAN, lockdep, 메모리 부족 주입은 아직 테스트하지 않았습니다.
