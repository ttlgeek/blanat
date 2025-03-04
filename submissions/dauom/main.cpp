#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>  // For close()

#include <algorithm>
#include <cstring>
#include <ext/pb_ds/assoc_container.hpp>
#include <future>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace __gnu_pbds;
using namespace std;

template <class K, class V>
using ht = gp_hash_table<K, V>;

#define NUM_CITIES 102
const char *INPUT_FILENAME = "input.txt";
const char *OUTPUT_FILENAME = "output.txt";
const int NUM_THREADS = thread::hardware_concurrency();

inline void handle_error(const char *msg) {
  perror(msg);
  exit(255);
}

struct MappedFile {
  const int fd;
  const size_t file_size;
  const char *file_data;
};

struct Result {
  long long city_cost[NUM_CITIES];
  long long product_cost[NUM_CITIES][NUM_CITIES];
  ht<string, int> product_id;
  ht<string, int> city_id;
  Result()
      : product_id({}, {}, {}, {}, {1 << 7}),
        city_id({}, {}, {}, {}, {1 << 7}) {
    fill_n(city_cost, NUM_CITIES, 0);
    fill_n((long long *)product_cost, NUM_CITIES * NUM_CITIES, (long long)4e18);
  }
};

inline const MappedFile map_input() {
  int fd = open(INPUT_FILENAME, O_RDONLY);
  if (fd == -1) handle_error("map_input: open failed");

  struct stat sb;
  if (fstat(fd, &sb) == -1) handle_error("map_input: fstat failed");

  const char *addr = static_cast<const char *>(
      mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0u));
  if (addr == MAP_FAILED) handle_error("map_input: mmap failed");
  madvise((void *)addr, sb.st_size, MADV_SEQUENTIAL);

  return {fd, (size_t)sb.st_size, addr};
}

inline const void consume_str(char *&start, string &s) {
  char c;
  while ((c = *start) != 0 && c != ',' && c != '\n') {
    s += c;
    ++start;
  }
  ++start;
}

inline long long consume_float_as_long(char *&start) {
  long long ans = 0;
  char c;

  // integral part
  while ((c = *start) <= '9' && c >= '0') {
    ans = ans * 10 + c - '0';
    ++start;
  }
  ++start;         // skip dot or \n
  if (c == '.') {  // fractional part exists, consume!
    int count = 0;
    while ((c = *start) <= '9' && c >= '0') {
      ans = ans * 10 + c - '0';
      ++start;
      ++count;
    }
    if (count == 1) ans *= 10;    // always precision=2
    ++start;                      // skip \n or \r
    if (*start == '\n') ++start;  // skip \n if prefixed by \r
  }
  return ans;
}

inline int find_or_create(ht<string, int> &id_map, const string &k) {
  int id = -1;
  if (id_map.find(k) == id_map.end()) {
    id = id_map[k] = id_map.size();
  } else {
    id = id_map[k];
  }
  return id;
}

class ThreadPool {
 public:
  Result *results;
  void start(const int num_threads);
  void queue_job(const function<void(Result &)> &job);
  void stop();
  bool busy();

 private:
  void loop(int id);

  bool should_terminate = false;
  mutex queue_mutex;
  condition_variable mutex_condition;
  vector<thread> threads;
  queue<function<void(Result &)>> jobs;
};

void ThreadPool::start(const int num_threads) {
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back(thread(&ThreadPool::loop, this, i));
  }
  results = new Result[num_threads];
}

void ThreadPool::loop(int id) {
  while (true) {
    function<void(Result &)> job;
    {
      unique_lock<mutex> lock(queue_mutex);
      mutex_condition.wait(
          lock, [this] { return !jobs.empty() || should_terminate; });
      if (should_terminate) {
        return;
      }
      job = jobs.front();
      jobs.pop();
    }
    job(results[id]);
  }
}

void ThreadPool::queue_job(const function<void(Result &)> &job) {
  {
    unique_lock<mutex> lock(queue_mutex);
    jobs.push(job);
  }
  mutex_condition.notify_one();
}

bool ThreadPool::busy() {
  bool poolbusy = false;
  {
    unique_lock<mutex> lock(queue_mutex);
    poolbusy = !jobs.empty();
  }
  return poolbusy;
}

void ThreadPool::stop() {
  {
    unique_lock<mutex> lock(queue_mutex);
    should_terminate = true;
  }
  mutex_condition.notify_all();
  for (thread &active_thread : threads) {
    active_thread.join();
  }
  threads.clear();
}

void process_chunk(Result &r, char *start, char *end) {
  if (*start != '\n') {
    start = (char *)rawmemchr(start, '\n');
  }
  ++start;

  char *cur = start;
  string city;
  city.reserve(40);
  string product;
  product.reserve(40);
  while (cur < end) {
    city.clear();
    consume_str(cur, city);
    product.clear();
    consume_str(cur, product);
    long long price = consume_float_as_long(cur);

    int cid = find_or_create(r.city_id, city);
    int pid = find_or_create(r.product_id, product);
    r.product_cost[cid][pid] = min(r.product_cost[cid][pid], price);
    r.city_cost[cid] += price;
  }
}

inline void process_concurrently(const MappedFile &mp,
                                 vector<Result> &results) {
  char *start = (char *)mp.file_data;
  char *end = start + mp.file_size;
  const size_t block_size = 1024 * 1024 * 100;  // 100mb
  const size_t chunks_count = mp.file_size / block_size;

  ThreadPool thread_pool;
  thread_pool.start(NUM_THREADS);
  for (size_t i = 0; i < chunks_count; i++) {
    char *chunk_start = start + i * block_size;
    char *chunk_end = start + (i + 1) * block_size;
    if (i + 1 == chunks_count) chunk_end = end;
    if (chunk_start >= chunk_end) break;

    thread_pool.queue_job([chunk_start, chunk_end](Result &r) {
      process_chunk(r, chunk_start, chunk_end);
    });
  }

  while (thread_pool.busy()) {
  }
  thread_pool.stop();
  for (int i = 0; i < NUM_THREADS; i++) {
    results.emplace_back(thread_pool.results[i]);
  }
}

inline Result merge(vector<Result> &results) {
  Result mr;
  for (auto &r : results) {
    for (auto &cid : r.city_id) {
      int ncid = find_or_create(mr.city_id, cid.first);
      mr.city_cost[ncid] += r.city_cost[cid.second];
      for (auto &pid : r.product_id) {
        int npid = find_or_create(mr.product_id, pid.first);
        mr.product_cost[ncid][npid] =
            min(mr.product_cost[ncid][npid],
                r.product_cost[cid.second][pid.second]);
      }
    }
  }
  return mr;
}

inline void ans(Result &result) {
  FILE *f = fopen(OUTPUT_FILENAME, "w");

  long long min_city_cost = (long long)4e18;
  string city = "";
  int city_id = -1;
  for (auto &cid : result.city_id) {
    long long c = result.city_cost[cid.second];
    if (min_city_cost > c) {
      min_city_cost = c;
      city = cid.first;
      city_id = cid.second;
    } else if (c == min_city_cost && city > cid.first) {
      city = cid.first;
      city_id = cid.second;
    }
  }
  fprintf(f, "%s %.2f\n", city.c_str(), min_city_cost / 100.0);

  vector<pair<long long, string>> products;
  for (auto &pid : result.product_id) {
    products.push_back({result.product_cost[city_id][pid.second], pid.first});
  }
  sort(products.begin(), products.end());
  products.resize(5);

  for (auto &p : products) {
    fprintf(f, "%s %.2f\n", p.second.c_str(), p.first / 100.0);
  }
  fflush(f);
  fclose(f);
}

int main() {
  const MappedFile mp = map_input();

  vector<Result> results;
  results.reserve(NUM_THREADS);
  process_concurrently(mp, results);
  Result result = merge(results);
  ans(result);

  // Unmap the memory and close the file
  munmap((void *)mp.file_data, mp.file_size);
  close(mp.fd);
  return 0;
}
