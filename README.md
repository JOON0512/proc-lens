# proc-lens

Linux 프로세스 정보를 `/proc/proc_lens/snapshot`에서 TSV로 읽는 커널 모듈.

PID, PPID, 이름, CPU 시간, 스케줄러 상태, RSS와 코드·데이터·힙의 가상주소 구간을 주기적으로 수집합니다. 파일을 열 때 스냅샷을 고정하므로 읽는 도중 수집 주기가 바뀌어도 출력은 유지됩니다.

## Run

Linux 6.8 / x86-64에서 테스트했습니다. 커널 모듈 실험용 VM에서 실행하세요. 조회에는 root와 `CAP_SYS_ADMIN` 권한이 필요합니다.

```sh
sudo apt install build-essential linux-headers-$(uname -r)
make
sudo insmod proc_lens.ko interval_ms=1000 max_tasks=4096
sudo cat /proc/proc_lens/snapshot
sudo rmmod proc_lens
```

`interval_ms`는 수집 간격, `max_tasks`는 수집할 프로세스 수의 상한입니다. 상한에 도달하면 헤더에 잘림 여부를 표시합니다.

## How it works

수집은 `delayed_work`에서 처리하고 task/mm 참조를 확보한 뒤 값을 복사합니다. 완성된 스냅샷을 mutex 안에서 교체하며, 열린 파일은 자기 스냅샷의 참조를 유지합니다. 모듈 제거 시에는 작업과 proc 콜백이 끝난 뒤 메모리를 해제합니다.

출력 필드와 QEMU 테스트 실행법은 [개발 노트](docs/development.md)에 있습니다.

GPL-2.0-only. 참고: [seq_file](https://docs.kernel.org/filesystems/seq_file.html), [workqueue](https://docs.kernel.org/core-api/workqueue.html).
