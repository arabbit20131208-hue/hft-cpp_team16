// tape_driver.cpp — paced, open-loop tape driver for any --replay bot.
//
// The course harness (scripts/latency_replay.py) is lock-step: it sends one
// line and waits for the answer, so at most one message is ever in flight and
// a pipeline cannot overlap stages. This driver instead writes the tape at a
// fixed rate (or as fast as possible with --rate 0) on one thread and reads
// the answers on another, like a busy venue would.
//
// Latency of message k = (time its answer line arrived) - (time message k was
// SCHEDULED to be sent). Measuring from the schedule, not from the actual
// write, means a bot that falls behind is charged for the queueing it causes
// (no coordinated omission).
//
// Usage:
//   tape_driver --tape busy.jsonl --rate 50000 [--warmup 1000] [--stderr FILE]
//               -- ./hft_bot --replay
// The tape holds one raw JSON message per line ({"msg": ...} wrappers are
// also accepted). Output: one summary line, plus --json FILE if given.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void die(const char* msg) {
  std::perror(msg);
  std::exit(2);
}

std::string unwrap(const std::string& line) {
  static const char kPrefix[] = "{\"msg\":";
  if (line.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return line;
  const std::size_t end = line.find_last_of('}');
  return line.substr(sizeof(kPrefix) - 1, end - (sizeof(kPrefix) - 1));
}

double pct(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(q / 100.0 * v.size()));
  return v[i];
}

}  // namespace

int main(int argc, char** argv) {
  std::string tape_path, err_path = "/dev/null", json_path;
  double rate = 0;
  std::size_t warmup = 1000;
  int cmd = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--") {
      cmd = i + 1;
      break;
    }
    if (i + 1 >= argc) break;
    if (a == "--tape") tape_path = argv[++i];
    else if (a == "--rate") rate = std::atof(argv[++i]);
    else if (a == "--warmup") warmup = std::strtoul(argv[++i], nullptr, 10);
    else if (a == "--stderr") err_path = argv[++i];
    else if (a == "--json") json_path = argv[++i];
  }
  if (tape_path.empty() || cmd < 0 || cmd >= argc) {
    std::fprintf(stderr, "usage: tape_driver --tape F --rate R [--warmup N] [--stderr F] "
                         "[--json F] -- BOT ARGS...\n");
    return 2;
  }

  std::vector<std::string> lines;
  {
    std::ifstream f(tape_path);
    std::string line;
    while (std::getline(f, line))
      if (!line.empty()) lines.push_back(unwrap(line) + "\n");
  }
  const std::size_t n = lines.size();
  if (n <= warmup) {
    std::fprintf(stderr, "tape has %zu lines, need more than --warmup %zu\n", n, warmup);
    return 2;
  }

  int to_bot[2], from_bot[2];
  if (pipe(to_bot) || pipe(from_bot)) die("pipe");
  const pid_t pid = fork();
  if (pid < 0) die("fork");
  if (pid == 0) {
    dup2(to_bot[0], 0);
    dup2(from_bot[1], 1);
    const int err = open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (err >= 0) dup2(err, 2);
    close(to_bot[1]);
    close(from_bot[0]);
    execvp(argv[cmd], argv + cmd);
    die("execvp");
  }
  close(to_bot[0]);
  close(from_bot[1]);

  std::vector<Clock::time_point> scheduled(n), answered(n);
  const auto t0 = Clock::now() + std::chrono::milliseconds(200);  // let the bot start
  const double gap_ns = rate > 0 ? 1e9 / rate : 0;

  std::thread writer([&] {
    for (std::size_t i = 0; i < n; ++i) {
      const auto due = t0 + std::chrono::nanoseconds(static_cast<long long>(i * gap_ns));
      if (rate > 0) {
        // Sleep until shortly before the deadline, then spin: keeps the
        // driver off the CPU so it does not steal cores from the bot.
        if (due - Clock::now() > std::chrono::microseconds(200))
          std::this_thread::sleep_until(due - std::chrono::microseconds(100));
        while (Clock::now() < due) {
        }
        scheduled[i] = due;
      } else {
        scheduled[i] = Clock::now();
      }
      const char* p = lines[i].data();
      std::size_t left = lines[i].size();
      while (left) {
        const ssize_t w = write(to_bot[1], p, left);
        if (w <= 0) die("write");
        p += w;
        left -= static_cast<std::size_t>(w);
      }
    }
    close(to_bot[1]);
  });

  std::size_t got = 0;
  std::vector<char> buf(1 << 16);
  while (got < n) {
    const ssize_t r = read(from_bot[0], buf.data(), buf.size());
    if (r <= 0) break;
    const auto now = Clock::now();
    for (ssize_t k = 0; k < r; ++k)
      if (buf[static_cast<std::size_t>(k)] == '\n' && got < n) answered[got++] = now;
  }
  writer.join();
  close(from_bot[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (got < n) {
    std::fprintf(stderr, "bot answered %zu of %zu lines\n", got, n);
    return 1;
  }

  std::vector<double> lat;
  lat.reserve(n - warmup);
  for (std::size_t i = warmup; i < n; ++i)
    lat.push_back(std::chrono::duration<double, std::micro>(answered[i] - scheduled[i]).count());
  const double secs = std::chrono::duration<double>(answered[n - 1] - scheduled[warmup]).count();
  const double offered = rate > 0 ? rate : 0;
  const double achieved = (n - warmup) / secs;
  const double final_lag = lat.back();
  const double p50 = pct(lat, 50), p90 = pct(lat, 90), p99 = pct(lat, 99), p999 = pct(lat, 99.9);
  const double mx = *std::max_element(lat.begin(), lat.end());
  std::printf("n=%zu offered=%.0f/s achieved=%.0f/s p50=%.2f p90=%.2f p99=%.2f p99.9=%.2f "
              "max=%.2f final_lag=%.2f us\n",
              n - warmup, offered, achieved, p50, p90, p99, p999, mx, final_lag);
  if (!json_path.empty()) {
    if (FILE* f = std::fopen(json_path.c_str(), "w")) {
      std::fprintf(f,
                   "{\"n\":%zu,\"offered_per_s\":%.0f,\"achieved_per_s\":%.0f,\"p50_us\":%.3f,"
                   "\"p90_us\":%.3f,\"p99_us\":%.3f,\"p999_us\":%.3f,\"max_us\":%.3f,"
                   "\"final_lag_us\":%.3f}\n",
                   n - warmup, offered, achieved, p50, p90, p99, p999, mx, final_lag);
      std::fclose(f);
    }
  }
  return 0;
}
