#include <aml.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "stream/stream.h"
#include "sys/queue.h"


static bool test_simple_tcp_stream(void)
{
  int sockets[2];
  struct stream* s = NULL;
  bool result = true;

  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    result = false;
    goto cleanup;
  }
  close(sockets[1]);

  void on_ss_event(struct stream*, enum stream_event) {}
  s = stream_new(sockets[0], on_ss_event, NULL);
  if (s == NULL) {
    result = false;
    close(sockets[0]);
    goto cleanup;
  }
  
  stream_close(s);
  stream_destroy(s);
  s = NULL;

cleanup:
  return result;
}

struct write_until_full_result {
  bool success;
  size_t bytes_buffered;
  size_t bytes_written;
  size_t num_writes;
};
static struct write_until_full_result write_until_full(
    struct stream* s, struct rcbuf* data, int additional_writes) {
  struct write_until_full_result result = {.success = true};
  const size_t data_size = data->size;

  if (!data) {
    result.success = false;
    return result;
  }

  // Write a bunch of useless data into the socket (until it becomes full).
  bool buffering_data = false;
  while (!buffering_data || additional_writes > 0) {
    rcbuf_ref(data);
    ssize_t r = stream_send(s, data, NULL, NULL);
    if (r < 0) {
      result.success = false;
      break;
    }
    result.bytes_buffered += r;
    result.bytes_written += data_size;
    result.num_writes++;
    if (r == 0) {
      buffering_data = true;
      additional_writes--;
    }
  }

  printf("Added %ldB to send queue\n", result.bytes_written);

  rcbuf_unref(data);
  return result;
}

static void do_nothing_handler(struct stream*, enum stream_event) {}
static void wrapped_socket_pair_read_handler(struct stream* stream, enum stream_event event) {
  if (event != STREAM_EVENT_READ)
    return;
  size_t* received_bytes = stream->userdata;
  char* tmp = malloc(4096);
  ssize_t r = stream_read(stream, tmp, 4096);
  if (r > 0) {
    *received_bytes += r;
  }
  free(tmp);
}

static bool wrapped_socket_pair(
    struct stream** write_stream, struct stream** read_stream,
    size_t* bytes_read) {
  *write_stream = NULL;
  *read_stream = NULL;

  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
    return false;
  }

  *write_stream = stream_new(sockets[0], do_nothing_handler, NULL);
  if (*write_stream == NULL) goto failure;

  *read_stream = stream_new(sockets[1], wrapped_socket_pair_read_handler, bytes_read);
  if (*read_stream == NULL) goto failure;

  return true;

failure:
  if (*write_stream == NULL) {
    close(sockets[0]);
  } else {
    stream_close(*write_stream);
    stream_destroy(*write_stream);
    *write_stream = NULL;
  }
  if (*read_stream == NULL) {
    close(sockets[1]);
  } else {
    stream_close(*read_stream);
    stream_destroy(*read_stream);
    *read_stream = NULL;
  }
  return false;
}

static bool test_tcp_stream_buffers_writes(int buffer_size)
{
  struct stream* ss = NULL;
  struct stream* rs = NULL;
  bool result = true;

  size_t received_bytes = 0;
  if (!wrapped_socket_pair(&ss, &rs, &received_bytes)) return false;

  struct rcbuf* data = rcbuf_new(calloc(buffer_size, 1), buffer_size);
  struct write_until_full_result write_result = write_until_full(
      ss, data, /*additional_writes=*/1);
  if (!write_result.success) {
    result = false;
    goto cleanup;
  }
  
  void timeout_expired(struct aml_timer*) {
    printf("Test timed out\n");
    result = false;
  }
  struct aml_timer* test_timeout = aml_timer_new(5*1000*1000, timeout_expired, NULL, NULL);
  aml_start(aml_get_default(), test_timeout);

  do {
    aml_poll(aml_get_default(), 10);
    aml_dispatch(aml_get_default());
  } while (result && received_bytes < write_result.bytes_written);

  result = result && write_result.bytes_written == received_bytes;

  printf("expected: %ld, got: %ld\n",
      write_result.bytes_written, received_bytes);

cleanup:
  aml_stop(aml_get_default(), test_timeout);
  aml_unref(test_timeout);
  stream_close(ss);
  stream_destroy(ss);
  stream_close(rs);
  stream_destroy(rs);
  return result;
}

static bool test_tcp_stream_write_in_callback(void)
{
  struct stream* ss = NULL;
  struct stream* rs = NULL;
  bool result = true;

  size_t received_bytes = 0;
  if (!wrapped_socket_pair(&ss, &rs, &received_bytes)) return false;

  void on_write_completed(void*, enum stream_req_status status) {
    if (status != STREAM_REQ_DONE) {
      printf("Outer write failed\n");
      result = false;
      return;
    }
    if (stream_send(ss, rcbuf_from_string("A"), NULL, NULL) < 0) {
      printf("Inner write failed\n");
      result = false;
    }
  }
  if (stream_send(ss, rcbuf_from_string("A"), on_write_completed, NULL) < 0) {
    printf("Outer write failed\n");
    result = false;
    goto cleanup;
  }
  const size_t expected_bytes = 2;
  
  void timeout_expired(struct aml_timer*) {
    printf("Test timed out\n");
    result = false;
  }
  struct aml_timer* test_timeout = aml_timer_new(5*1000*1000, timeout_expired, NULL, NULL);
  aml_start(aml_get_default(), test_timeout);

  do {
    aml_poll(aml_get_default(), 10);
    aml_dispatch(aml_get_default());
  } while (result && received_bytes < expected_bytes);

  result = result && expected_bytes == received_bytes;

  printf("expected: %ld, got: %ld\n", expected_bytes, received_bytes);

cleanup:
  aml_stop(aml_get_default(), test_timeout);
  aml_unref(test_timeout);
  stream_close(ss);
  stream_destroy(ss);
  stream_close(rs);
  stream_destroy(rs);
  return result;
}

static bool test_tcp_stream_exec_send(int buffer_size, int num_buffers)
{
  struct stream* ss = NULL;
  struct stream* rs = NULL;
  bool result = true;

  size_t received_bytes = 0;
  if (!wrapped_socket_pair(&ss, &rs, &received_bytes)) return false;

  size_t expected_bytes = 0;
  size_t write_cb_called = 0;
  struct rcbuf* data = rcbuf_new(calloc(buffer_size, 1), buffer_size);

  struct rcbuf* write_cb(struct stream*, void* userdata) {
    write_cb_called++;
    rcbuf_ref(data);
    return data;
  }
  for (int i = 0; i < num_buffers; ++i) {
    stream_exec_and_send(ss, write_cb, calloc(1, 1));
    expected_bytes += buffer_size;
  }
  
  void timeout_expired(struct aml_timer*) {
    printf("Test timed out\n");
    result = false;
  }
  struct aml_timer* test_timeout = aml_timer_new(5*1000*1000, timeout_expired, NULL, NULL);
  aml_start(aml_get_default(), test_timeout);

  do {
    aml_poll(aml_get_default(), 10);
    aml_dispatch(aml_get_default());
  } while (result && received_bytes < expected_bytes);

  result = result && expected_bytes == received_bytes;

  printf("[bytes] expected: %ld, got: %ld\n", expected_bytes, received_bytes);
  printf("[callbacks] expected: %d, got: %ld\n", num_buffers, write_cb_called);

cleanup:
  rcbuf_unref(data);
  aml_stop(aml_get_default(), test_timeout);
  aml_unref(test_timeout);
  stream_close(ss);
  stream_destroy(ss);
  stream_close(rs);
  stream_destroy(rs);
  return result;
}

static bool test_tcp_stream_exec_send_close()
{
  struct stream* ss = NULL;
  struct stream* rs = NULL;
  bool result = true;

  size_t received_bytes = 0;
  if (!wrapped_socket_pair(&ss, &rs, &received_bytes)) return false;

  size_t write_cb_called = 0;
  struct rcbuf* write_cb(struct stream*, void* userdata) {
    write_cb_called++;
    return rcbuf_from_string("A");
  }
  void on_write_completed(void*, enum stream_req_status status) {
    stream_exec_and_send(ss, write_cb, calloc(1, 1));
  }
  if (stream_send(ss, rcbuf_new(malloc(1), 0), on_write_completed, NULL) < 0) {
    result = false;
    goto cleanup;
  }
  stream_close(ss);

  printf("[callback] write_function: %ld\n", write_cb_called);

cleanup:
  stream_close(ss);
  stream_destroy(ss);
  stream_close(rs);
  stream_destroy(rs);
  return result;
}


#define RUN_TEST(name, ...) ({ \
	printf("[----] %s(%s)\n", "" #name, "" #__VA_ARGS__); \
  struct aml* aml = aml_new(); \
  aml_set_default(aml); \
	bool ok = test_ ## name(__VA_ARGS__); \
  aml_set_default(NULL); \
  aml_unref(aml); \
	printf("[%s] %s(%s)\n", ok ? " OK " : "FAIL", "" #name, "" #__VA_ARGS__); \
	ok; \
})
int main()
{
	bool ok = true;

	ok &= RUN_TEST(simple_tcp_stream);
	ok &= RUN_TEST(
	    tcp_stream_buffers_writes, /*buffer_size=*/1);
	ok &= RUN_TEST(
	    tcp_stream_buffers_writes, /*buffer_size=*/1<<20);
	ok &= RUN_TEST(tcp_stream_write_in_callback);
	ok &= RUN_TEST(tcp_stream_exec_send, /*buffer_size=*/1, 300);
	ok &= RUN_TEST(tcp_stream_exec_send, /*buffer_size=*/1<<20, 10);
	ok &= RUN_TEST(tcp_stream_exec_send_close);

	return ok ? 0 : 1;
}
