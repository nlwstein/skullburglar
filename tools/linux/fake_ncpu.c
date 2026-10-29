// LD_PRELOAD shim: make the process see FAKE_NCPU (default 4) logical CPUs, to test code paths for small machines.
// libstdc++'s std::thread::hardware_concurrency ignores taskset, so pair it with `taskset -c 0-3` for real contention.
//   clang -shared -fPIC tools/linux/fake_ncpu.c -o fake_ncpu.so -ldl
//   LD_PRELOAD=$PWD/fake_ncpu.so FAKE_NCPU=4 taskset -c 0-3 ./bots ...
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <sys/sysinfo.h>
#include <unistd.h>
static int ncpu(void) {
  const char* e = getenv("FAKE_NCPU");
  return e ? atoi(e) : 4;
}
int get_nprocs(void) { return ncpu(); }
int get_nprocs_conf(void) { return ncpu(); }
long sysconf(int name) {
  if (name == _SC_NPROCESSORS_ONLN || name == _SC_NPROCESSORS_CONF) return ncpu();
  long (*real)(int) = (long (*)(int))dlsym(RTLD_NEXT, "sysconf");
  return real(name);
}
