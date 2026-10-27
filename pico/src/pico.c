// lhsyncd's PICO 4 Pro side (src/headset/headset.h): Android properties, the factory camera calibration in
// /mnt/vendor/persist/pvr/mav0 (Kalibr/EuRoC sensor.yaml files, omnidirectional camera model) and lhsight as a Frida
// script (frida-inject + lhsight.js) inside pvrtrackingservice, which maps the tracking cameras' dmabuf rings: it reads
// them and writes nothing.
//
// Built without Bionic-only APIs (no system_properties.h, no liblog), so it can be a static binary (musl) as well as an
// NDK one: properties come from /system/bin/getprop, and the log goes to a file in the run dir.
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "headset.h"

#define HAL "pvrtrackingservice"
#ifndef RUNDIR
#define RUNDIR "/dev/.questlhsync"
#endif
#define LOGFILE RUNDIR "/lhsyncd.log"
#ifndef CALDIR
#define CALDIR "/mnt/vendor/persist/pvr/mav0"
#endif
#define NCAM 4

const char *const hs_capture_name = "frida";
static const char *moddir = "/data/adb/modules/questlhsync_pico";

void hs_log(const char *line) {
  mkdir(RUNDIR, 0755);
  struct stat st;
  if (!stat(LOGFILE, &st) && st.st_size > 512 * 1024) rename(LOGFILE, LOGFILE ".1");
  FILE *f = fopen(LOGFILE, "a");
  if (!f) return;
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char ts[32];
  strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &tm);
  fprintf(f, "%s %s\n", ts, line);
  fclose(f);
}

static void logf_(const char *fmt, ...) {
  char b[700];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  hs_log(b);
}

// a property through /system/bin/getprop (no Bionic property API in a static binary)
static void getprop(const char *name, char *out, size_t n) {
  out[0] = 0;
  int p[2];
  if (pipe(p)) return;
  pid_t pid = fork();
  if (pid == 0) {
    dup2(p[1], 1);
    close(p[0]);
    close(p[1]);
    int nul = open("/dev/null", O_RDWR);
    dup2(nul, 0);
    dup2(nul, 2);
    execl("/system/bin/getprop", "getprop", name, (char *)NULL);
    _exit(127);
  }
  close(p[1]);
  size_t got = 0;
  if (pid > 0) {
    ssize_t r;
    while (got + 1 < n && (r = read(p[0], out + got, n - 1 - got)) > 0) got += (size_t)r;
    waitpid(pid, NULL, 0);
  }
  close(p[0]);
  out[got] = 0;
  while (got && (out[got - 1] == '\n' || out[got - 1] == '\r' || out[got - 1] == ' ')) out[--got] = 0;
}

void hs_device_info(char *serial, char *model, char *fw, size_t n) {
  getprop("ro.serialno", serial, n);
  getprop("ro.product.model", model, n);
  getprop("ro.build.display.id", fw, n);
  if (!model[0]) snprintf(model, n, "PICO 4 Pro");
}

// a whole file, NUL-terminated (malloc'd), or NULL
static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc(n > 0 ? (size_t)n + 1 : 1);
  size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
  fclose(f);
  buf[got] = 0;
  if (len) *len = got;
  return buf;
}

// ---------------------------------------------------------------- the camera calibration
// cam<i>/sensor.yaml (Kalibr, as PICO's factory writes it):
//   T_BS: {cols: 4, rows: 4, data: [16 numbers]}  the camera -> IMU (body) transform
//   resolution: [640, 480]   camera_model: omni   intrinsics: [xi, fu, fv, pu, pv]
//   distortion_model: radtan   distortion_coefficients: [k1, k2, p1, p2]
struct kcam {
  double T[16], xi, fu, fv, cu, cv, d[4];
  int w, h;
};

// the numbers of "key: [a, b, ...]" (the first such key after from), up to max; their count, or -1
static int yaml_list(const char *s, const char *key, double *v, int max) {
  const char *p = strstr(s, key);
  if (!p) return -1;
  p = strchr(p, '[');
  if (!p) return -1;
  int n = 0;
  for (p++; n < max;) {
    while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p == ']' || !*p) break;
    char *e;
    double x = strtod(p, &e);
    if (e == p) break;
    v[n++] = x;
    p = e;
  }
  return n;
}

static int load_cam(int i, struct kcam *c, char *why, size_t n) {
  char path[256];
  snprintf(path, sizeof path, CALDIR "/cam%d/sensor.yaml", i);
  char *s = read_file(path, NULL);
  if (!s) { snprintf(why, n, "can't read %s", path); return -1; }
  double res[2], in[5];
  int ok = yaml_list(strstr(s, "T_BS") ? strstr(s, "T_BS") : s, "data:", c->T, 16) == 16 &&
           yaml_list(s, "resolution:", res, 2) == 2 && yaml_list(s, "intrinsics:", in, 5) == 5 &&
           yaml_list(s, "distortion_coefficients:", c->d, 4) == 4 && strstr(s, "camera_model: omni") &&
           strstr(s, "distortion_model: radtan");
  free(s);
  if (!ok) { snprintf(why, n, "%s isn't an omni/radtan Kalibr camera", path); return -1; }
  c->w = (int)res[0];
  c->h = (int)res[1];
  c->xi = in[0]; c->fu = in[1]; c->fv = in[2]; c->cu = in[3]; c->cv = in[4];
  return 0;
}

// the omnidirectional (Mei) model with radial-tangential distortion: a camera-frame direction -> pixel
static int omni(const struct kcam *c, const double X[3], double *u, double *v) {
  double r = sqrt(X[0] * X[0] + X[1] * X[1] + X[2] * X[2]), den = X[2] / r + c->xi;
  if (den <= 1e-9) return -1;
  double mx = X[0] / r / den, my = X[1] / r / den, r2 = mx * mx + my * my;
  double rad = 1 + c->d[0] * r2 + c->d[1] * r2 * r2;
  double xd = mx * rad + 2 * c->d[2] * mx * my + c->d[3] * (r2 + 2 * mx * mx);
  double yd = my * rad + c->d[2] * (r2 + 2 * my * my) + 2 * c->d[3] * mx * my;
  *u = c->fu * xd + c->cu;
  *v = c->fv * yd + c->cv;
  return 0;
}

// Meta's Fisheye62 as the driver evaluates it (CamCal::Dist): P = f, cx, cy, k1..k6, p1, p2
#define NP 11
static void f62(const double *P, const double X[3], double *u, double *v) {
  double a = X[0] / X[2], b = X[1] / X[2], r = hypot(a, b), th = atan(r), t2 = th * th;
  double thd = th * (1 + t2 * (P[3] + t2 * (P[4] + t2 * (P[5] + t2 * (P[6] + t2 * (P[7] + t2 * P[8]))))));
  double s = r > 1e-12 ? thd / r : 1.0, uu = a * s, vv = b * s, r2 = uu * uu + vv * vv, d = 2 * (uu * P[9] + vv * P[10]);
  *u = P[0] * (uu + d * uu + r2 * P[9]) + P[1];
  *v = P[0] * (vv + d * vv + r2 * P[10]) + P[2];
}

// solves A x = b (n x n, row-major) in place by Gaussian elimination with partial pivoting; 0 on success
static int solve(double *A, double *b, int n) {
  for (int c = 0; c < n; c++) {
    int piv = c;
    for (int r = c + 1; r < n; r++) if (fabs(A[r * n + c]) > fabs(A[piv * n + c])) piv = r;
    if (fabs(A[piv * n + c]) < 1e-300) return -1;
    if (piv != c) {
      for (int k = 0; k < n; k++) { double t = A[c * n + k]; A[c * n + k] = A[piv * n + k]; A[piv * n + k] = t; }
      double t = b[c]; b[c] = b[piv]; b[piv] = t;
    }
    for (int r = c + 1; r < n; r++) {
      double f = A[r * n + c] / A[c * n + c];
      for (int k = c; k < n; k++) A[r * n + k] -= f * A[c * n + k];
      b[r] -= f * b[c];
    }
  }
  for (int c = n - 1; c >= 0; c--) {
    for (int k = c + 1; k < n; k++) b[c] -= A[c * n + k] * b[k];
    b[c] /= A[c * n + c];
  }
  return 0;
}

// The driver reads Meta's Fisheye62. The omni model is fitted to it over the rays that land on the image (in front of
// the camera, up to 84 degrees off axis): Levenberg-Marquardt on the pixel residuals. Returns the RMS in pixels.
static double fit_f62(const struct kcam *c, double *P) {
  enum { NT = 42, NPH = 64 };
  static double X[NT * NPH][3], U[NT * NPH][2];
  int m = 0;
  for (int i = 0; i < NT; i++)
    for (int j = 0; j < NPH; j++) {
      double th = 1.47 * i / (NT - 1), ph = 2 * M_PI * j / NPH, d[3] = { sin(th) * cos(ph), sin(th) * sin(ph), cos(th) };
      double u, v;
      if (omni(c, d, &u, &v) || u < 0 || v < 0 || u >= c->w || v >= c->h) continue;
      memcpy(X[m], d, sizeof d);
      U[m][0] = u;
      U[m][1] = v;
      m++;
      if (i == 0) break;  // the axis once
    }
  double init[NP] = { c->fu / (1 + c->xi), c->cu, c->cv, 0, 0, 0, 0, 0, 0, 0, 0 };
  memcpy(P, init, sizeof init);
  double lambda = 1e-3, cost = 0;
  for (int k = 0; k < m; k++) {
    double u, v;
    f62(P, X[k], &u, &v);
    cost += (u - U[k][0]) * (u - U[k][0]) + (v - U[k][1]) * (v - U[k][1]);
  }
  for (int it = 0; it < 200; it++) {
    double A[NP * NP] = { 0 }, g[NP] = { 0 };
    for (int k = 0; k < m; k++) {
      double u, v, J[2][NP];
      f62(P, X[k], &u, &v);
      for (int q = 0; q < NP; q++) {
        double Q[NP], h = 1e-7 * (fabs(P[q]) > 1 ? fabs(P[q]) : 1), u2, v2;
        memcpy(Q, P, sizeof Q);
        Q[q] += h;
        f62(Q, X[k], &u2, &v2);
        J[0][q] = (u2 - u) / h;
        J[1][q] = (v2 - v) / h;
      }
      double r[2] = { U[k][0] - u, U[k][1] - v };
      for (int a = 0; a < NP; a++) {
        g[a] += J[0][a] * r[0] + J[1][a] * r[1];
        for (int b = 0; b < NP; b++) A[a * NP + b] += J[0][a] * J[0][b] + J[1][a] * J[1][b];
      }
    }
    for (;;) {
      double B[NP * NP], dx[NP], Q[NP], nc = 0;
      memcpy(B, A, sizeof B);
      memcpy(dx, g, sizeof dx);
      for (int a = 0; a < NP; a++) B[a * NP + a] *= 1 + lambda;
      if (solve(B, dx, NP)) { lambda *= 10; if (lambda > 1e12) goto done; continue; }
      for (int a = 0; a < NP; a++) Q[a] = P[a] + dx[a];
      for (int k = 0; k < m; k++) {
        double u, v;
        f62(Q, X[k], &u, &v);
        nc += (u - U[k][0]) * (u - U[k][0]) + (v - U[k][1]) * (v - U[k][1]);
      }
      if (nc < cost) {
        int small = cost - nc < 1e-12 * cost;
        memcpy(P, Q, sizeof Q);
        cost = nc;
        lambda = lambda > 1e-9 ? lambda / 10 : lambda;
        if (small) goto done;
        break;
      }
      lambda *= 10;
      if (lambda > 1e12) goto done;
    }
  }
done:
  return m ? sqrt(cost / m) : -1;
}

static size_t jnum(char *o, size_t cap, const double *v, int n) {
  size_t k = 0;
  for (int i = 0; i < n && k + 32 < cap; i++) k += (size_t)snprintf(o + k, cap - k, "%s%.12g", i ? ", " : "", v[i]);
  return k;
}

// {"Device": {...}, "CameraCalibration": [Meta-style cameras: the fitted Fisheye62, DeviceFromCamera = T_BS],
//  "PicoKalibr": [the factory omni/radtan numbers as they are]}. DeviceFromCamera is in the IMU (body) frame.
int hs_calibration(char **data, size_t *len, char *name, size_t n) {
  struct kcam c[NCAM];
  for (int i = 0; i < NCAM; i++)
    if (load_cam(i, &c[i], name, n)) { *data = NULL; return -1; }
  size_t cap = 64 * 1024, o = 0;
  char *j = malloc(cap);
  char model[96], fw[96], serial[96];
  hs_device_info(serial, model, fw, sizeof model);
  o += (size_t)snprintf(j + o, cap - o, "{\"Device\": {\"DeviceType\": \"PICO 4 Pro\", \"Model\": \"%s\"},\n \"Frame\": \"imu\",\n"
                        " \"CameraCalibration\": [", model);
  for (int i = 0; i < NCAM; i++) {
    double P[NP], rms = fit_f62(&c[i], P);
    logf_("cam%d: omni xi %.3f f %.1f -> Fisheye62 f %.2f, fit RMS %.3f px", i, c[i].xi, c[i].fu, P[0], rms);
    o += (size_t)snprintf(j + o, cap - o, "%s\n  {\"Id\": \"%d\", \"SensorType\": \"OV9282\", \"Shutter\": {\"Type\": \"Global\"}, "
                          "\"ImageSize\": [%d, %d], \"Projection\": {\"Model\": \"Pinhole\", \"Coefficients\": [", i ? "," : "", i,
                          c[i].w, c[i].h);
    o += jnum(j + o, cap - o, P, 3);
    o += (size_t)snprintf(j + o, cap - o, "]}, \"Distortion\": {\"Model\": \"Fisheye62\", \"Coefficients\": [");
    o += jnum(j + o, cap - o, P + 3, 8);
    o += (size_t)snprintf(j + o, cap - o, "]}, \"FitRmsPx\": %.4f, \"DeviceFromCamera\": [", rms);
    o += jnum(j + o, cap - o, c[i].T, 16);
    o += (size_t)snprintf(j + o, cap - o, "]}");
  }
  o += (size_t)snprintf(j + o, cap - o, "],\n \"PicoKalibr\": [");
  for (int i = 0; i < NCAM; i++) {
    double in[5] = { c[i].xi, c[i].fu, c[i].fv, c[i].cu, c[i].cv };
    o += (size_t)snprintf(j + o, cap - o, "%s\n  {\"Id\": \"%d\", \"Resolution\": [%d, %d], \"Omni\": [", i ? "," : "", i, c[i].w,
                          c[i].h);
    o += jnum(j + o, cap - o, in, 5);
    o += (size_t)snprintf(j + o, cap - o, "], \"RadTan\": [");
    o += jnum(j + o, cap - o, c[i].d, 4);
    o += (size_t)snprintf(j + o, cap - o, "], \"T_BS\": [");
    o += jnum(j + o, cap - o, c[i].T, 16);
    o += (size_t)snprintf(j + o, cap - o, "]}");
  }
  o += (size_t)snprintf(j + o, cap - o, "]}\n");
  *data = j;
  *len = o;
  snprintf(name, n, "mav0");
  return 0;
}

// ---------------------------------------------------------------- lhsight
static pid_t hal_pid(void) {
  DIR *d = opendir("/proc");
  if (!d) return 0;
  struct dirent *e;
  pid_t found = 0;
  while (!found && (e = readdir(d))) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    char p[64], cmd[256];
    snprintf(p, sizeof p, "/proc/%.40s/cmdline", e->d_name);
    int fd = open(p, O_RDONLY);
    if (fd < 0) continue;
    ssize_t r = read(fd, cmd, sizeof cmd - 1);
    close(fd);
    if (r <= 0) continue;
    cmd[r] = 0;
    const char *base = strrchr(cmd, '/') ? strrchr(cmd, '/') + 1 : cmd;
    if (!strcmp(base, HAL)) found = atoi(e->d_name);
  }
  closedir(d);
  return found;
}

// cammap.txt in the module dir: the camera id of each ring in buffer order, "0,1,2,3" until it's been checked
static void cammap(char *out, size_t n) {
  char path[512];
  snprintf(path, sizeof path, "%s/cammap.txt", moddir);
  char *s = read_file(path, NULL);
  snprintf(out, n, "0,1,2,3");
  if (!s) return;
  size_t k = 0;
  for (const char *p = s; *p && k + 1 < n; p++)
    if ((*p >= '0' && *p <= '9') || *p == ',' || *p == '-') out[k++] = *p;
  if (k) out[k] = 0;
  free(s);
}

static void prepare_script(char *out, size_t cap, char *note, size_t ncap) {
  char src[512], map[64];
  size_t len;
  out[0] = 0;
  snprintf(src, sizeof src, "%s/lhsight.js", moddir);
  char *js = read_file(src, &len);
  if (!js) { snprintf(note, ncap, "can't read %s", src); return; }
  cammap(map, sizeof map);
  snprintf(note, ncap, "camera rings -> ids %s", map);
  snprintf(out, cap, "%s/lhsight.js", RUNDIR);
  FILE *f = fopen(out, "wb");
  if (!f) { snprintf(note, ncap, "can't write %s", out); out[0] = 0; free(js); return; }
  char *at = strstr(js, "@CAMMAP@");
  if (at) {
    fwrite(js, 1, (size_t)(at - js), f);
    fputs(map, f);
    fputs(at + 8, f);
  } else fwrite(js, 1, len, f);
  fclose(f);
  chmod(out, 0644);
  free(js);
}

// a frida-inject still running a lhsight.js (this daemon SIGKILLed, an old adb session): stop those, and only those
static int is_lhsight(const char *pid) {
  char p[64], cmd[1024];
  snprintf(p, sizeof p, "/proc/%s/cmdline", pid);
  int fd = open(p, O_RDONLY);
  if (fd < 0) return 0;
  ssize_t len = read(fd, cmd, sizeof cmd - 1);
  close(fd);
  if (len <= 0) return 0;
  cmd[len] = 0;
  const char *base = strrchr(cmd, '/') ? strrchr(cmd, '/') + 1 : cmd;
  if (strcmp(base, "frida-inject")) return 0;
  for (ssize_t i = (ssize_t)strlen(cmd) + 1; i < len; i += (ssize_t)strlen(cmd + i) + 1) {
    size_t l = strlen(cmd + i);
    if (l >= 10 && !strcmp(cmd + i + l - 10, "lhsight.js")) return 1;
  }
  return 0;
}

static void kill_strays(void) {
  DIR *d = opendir("/proc");
  if (!d) return;
  struct dirent *e;
  char found[16][16];
  int n = 0;
  while (n < 16 && (e = readdir(d)))
    if (e->d_name[0] >= '0' && e->d_name[0] <= '9' && is_lhsight(e->d_name)) snprintf(found[n++], 16, "%.15s", e->d_name);
  closedir(d);
  for (int i = 0; i < n; i++) {
    logf_("stopping a stray lhsight (frida-inject pid %s)", found[i]);
    kill(atoi(found[i]), SIGTERM);
  }
  for (int t = 0; n && t < 30; t++) {
    usleep(100000);
    int left = 0;
    for (int i = 0; i < n; i++) left += is_lhsight(found[i]);
    if (!left) return;
  }
  for (int i = 0; i < n; i++)
    if (is_lhsight(found[i])) kill(atoi(found[i]), SIGKILL);
}

void hs_init(int argc, char **argv) {
  if (argc > 1) moddir = argv[1];
  kill_strays();
  logf_("module %s (%s)", moddir, MODULE_VERSION);
}

pid_t hs_capture_start(int out_fd, char *msg, size_t n) {
  pid_t hal = hal_pid();
  if (!hal) { snprintf(msg, n, HAL " not running"); return 0; }
  char frida[512], script[512], pidstr[16], note[600];
  mkdir(RUNDIR, 0755);
  mkdir(RUNDIR "/tmp", 0755);
  prepare_script(script, sizeof script, note, sizeof note);
  if (!script[0]) { snprintf(msg, n, "%s", note); return 0; }
  snprintf(frida, sizeof frida, "%s/frida-inject", moddir);
  snprintf(pidstr, sizeof pidstr, "%d", hal);
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    dup2(out_fd, 1);
    dup2(out_fd, 2);
    int nul = open("/dev/null", O_RDONLY);
    dup2(nul, 0);
    for (int fd = 3; fd < 1024; fd++) close(fd);
    setenv("TMPDIR", RUNDIR "/tmp", 1);  // frida's scratch files: tmpfs, never /data/local/tmp
    if (chdir(RUNDIR)) _exit(126);
    execl(frida, "frida-inject", "-p", pidstr, "-s", script, (char *)NULL);
    _exit(127);
  }
  if (pid < 0) return -1;
  snprintf(msg, n, "lhsight started in " HAL " (pid %d); %s", hal, note);
  return pid;
}
