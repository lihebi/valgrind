/* Unit test for drd that triggers a race on the use of a POSIX condition
   variable. By Bart Van Assche.
*/

#include <cassert>
#include <cstdio>      // printf()
#include <pthread.h>
#include <unistd.h>    // usleep()
#include "../drd_clientreq.h"


// Local functions declarations.

static void* thread_func(void*);


// Local variables.

static pthread_mutex_t s_mutex;
static pthread_cond_t  s_cond;
static bool   s_use_mutex = false;


class CScopedLock
{
public:
  CScopedLock()
  { if (s_use_mutex) pthread_mutex_lock(&s_mutex); }
  ~CScopedLock()
  { if (s_use_mutex) pthread_mutex_unlock(&s_mutex); }

private:
  CScopedLock(CScopedLock const&);
  CScopedLock& operator=(CScopedLock const&);
};


// Function definitions.

static void set_thread_name(const char* const name)
{
  int res;
  VALGRIND_DO_CLIENT_REQUEST(res, 0, VG_USERREQ__SET_THREAD_NAME,
                             "%s", name, 0, 0, 0);
}

int main(int argc, char** argv)
{
  set_thread_name("main");

  int optchar;
  while ((optchar = getopt(argc, argv, "m")) != EOF)
  {
    switch (optchar)
    {
    case 'm':
      s_use_mutex = true;
      break;
    default:
      assert(false);
    }
  }

  pthread_cond_init(&s_cond, 0);
  pthread_mutex_init(&s_mutex, 0);
  pthread_mutex_lock(&s_mutex);

  pthread_t threadid;
  pthread_create(&threadid, 0, thread_func, 0);

  pthread_cond_wait(&s_cond, &s_mutex);
  pthread_mutex_unlock(&s_mutex);

  pthread_join(threadid, 0);

  pthread_mutex_destroy(&s_mutex);
  pthread_cond_destroy(&s_cond);

  return 0;
}

static void* thread_func(void*)
{
  set_thread_name("thread_func");

  // Wait until the main thread has entered pthread_cond_wait().
  pthread_mutex_lock(&s_mutex);
  pthread_mutex_unlock(&s_mutex);

  // Signal the condition variable.
  if (s_use_mutex) pthread_mutex_lock(&s_mutex);
  pthread_cond_signal(&s_cond);
  if (s_use_mutex) pthread_mutex_unlock(&s_mutex);

  return 0;
}
