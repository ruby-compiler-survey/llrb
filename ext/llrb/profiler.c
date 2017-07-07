#include <stdbool.h>
#include <signal.h>
#include <sys/time.h>
#include <pthread.h>
#include "ruby.h"
#include "ruby/debug.h"
#include "cruby.h"

#define LLRB_PROFILE_LIMIT 2048
#define LLRB_ENQUEUE_INTERVAL 40

struct llrb_sample {
  size_t score; // Total count of callstack depth
  bool compiled;
  const rb_callable_method_entry_t *cme;
};

static struct {
  int running;
  size_t profile_times;
  st_table *sample_by_frame; // Hash table: { frame => frame_data sample }
} llrb_profiler;

static VALUE gc_hook;

void
llrb_dump_iseq(const rb_iseq_t *iseq, const struct llrb_sample *sample)
{
  const rb_callable_method_entry_t *cme = sample->cme;

  fprintf(stderr, "%6ld: ", sample->score);
  switch (iseq->body->type) {
    case ISEQ_TYPE_METHOD:
      fprintf(stderr,"ISEQ_TYPE_METHOD:");
      break;
    case ISEQ_TYPE_CLASS:
      fprintf(stderr,"ISEQ_TYPE_CLASS:");
      break;
    case ISEQ_TYPE_BLOCK:
      fprintf(stderr,"ISEQ_TYPE_BLOCK:");
      break;
    case ISEQ_TYPE_EVAL:
      fprintf(stderr,"ISEQ_TYPE_EVAL:");
      break;
    case ISEQ_TYPE_MAIN:
      fprintf(stderr,"ISEQ_TYPE_MAIN:");
      break;
    case ISEQ_TYPE_TOP:
      fprintf(stderr,"ISEQ_TYPE_TOP:");
      break;
    case ISEQ_TYPE_RESCUE:
      fprintf(stderr,"ISEQ_TYPE_RESCUE:");
      break;
    case ISEQ_TYPE_ENSURE:
      fprintf(stderr,"ISEQ_TYPE_ENSURE:");
      break;
    case ISEQ_TYPE_DEFINED_GUARD:
      fprintf(stderr,"ISEQ_TYPE_DEFINED_GUARD:");
      break;
    default:
      fprintf(stderr,"default:");
      break;
  }

  if (cme && cme->def->type == VM_METHOD_TYPE_ISEQ) {
    VALUE name = rb_profile_frame_full_label((VALUE)cme);
    fprintf(stderr, " %s", RSTRING_PTR(name));
  }
  fprintf(stderr, "\n");
}

static struct llrb_sample *
llrb_sample_for(const rb_iseq_t *iseq, const rb_control_frame_t *cfp)
{
  st_data_t key = (st_data_t)iseq, val = 0;
  struct llrb_sample *sample;

  if (st_lookup(llrb_profiler.sample_by_frame, key, &val)) {
    sample = (struct llrb_sample *)val;
  } else {
    sample = ALLOC_N(struct llrb_sample, 1); // Not freed
    *sample = (struct llrb_sample){
      .score = 0,
      .compiled = false,
      .cme = rb_vm_frame_method_entry(cfp),
    };
    val = (st_data_t)sample;
    st_insert(llrb_profiler.sample_by_frame, key, val);
  }

  return sample;
}

static void
llrb_profile_frames()
{
  rb_thread_t *th = GET_THREAD();
  rb_control_frame_t *cfp = RUBY_VM_NEXT_CONTROL_FRAME(RUBY_VM_END_CONTROL_FRAME(th)); // reverse lookup
  rb_control_frame_t *end_cfp = th->cfp;

  for (int i = 0; i < LLRB_PROFILE_LIMIT && cfp != end_cfp; i++) {
    if (cfp->iseq) {
      struct llrb_sample *sample = llrb_sample_for(cfp->iseq, cfp);
      sample->score += i;
    }
    cfp = RUBY_VM_NEXT_CONTROL_FRAME(cfp);
  }

  llrb_profiler.profile_times++;
}

struct llrb_compile_target {
  const rb_iseq_t *iseq;
  struct llrb_sample* sample;
};

static int
llrb_search_compile_target_i(st_data_t key, st_data_t val, st_data_t arg)
{
  struct llrb_compile_target *target = (struct llrb_compile_target *)arg;
  const rb_iseq_t *iseq = (const rb_iseq_t *)key;
  struct llrb_sample *sample = (struct llrb_sample *)val;

  if (sample->compiled) return ST_CONTINUE;

  if (iseq->body->type == ISEQ_TYPE_METHOD || iseq->body->type == ISEQ_TYPE_BLOCK) {
    if (!target->iseq && !target->sample) {
      target->iseq = iseq;
      target->sample = sample;
    }
    if (sample->score > target->sample->score) {
      target->iseq = iseq;
      target->sample = sample;
    }
  }

  return ST_CONTINUE;
}

static const rb_iseq_t *
llrb_search_compile_target()
{
  struct llrb_compile_target target = (struct llrb_compile_target){
    .sample = 0,
    .iseq = 0,
  };
  st_foreach(llrb_profiler.sample_by_frame, llrb_search_compile_target_i, (st_data_t)&target);

  if (target.sample) {
    target.sample->compiled = true;
  }
  return target.iseq;
}

static void
llrb_job_handler(void *data)
{
  static int in_job_handler = 0;
  if (in_job_handler) return;
  if (!llrb_profiler.running) return;

  in_job_handler++;
  llrb_profile_frames();

  if (llrb_profiler.profile_times % LLRB_ENQUEUE_INTERVAL == 0) {
    const rb_iseq_t *target_iseq = llrb_search_compile_target();
    st_data_t val;
    st_lookup(llrb_profiler.sample_by_frame, (st_data_t)target_iseq, &val);
    struct llrb_sample *sample = (struct llrb_sample *)val;
    llrb_dump_iseq(target_iseq, sample);
  }
  in_job_handler--;
}

static void
llrb_signal_handler(int sig, siginfo_t *sinfo, void *ucontext)
{
  if (!rb_during_gc()) {
    rb_postponed_job_register_one(0, llrb_job_handler, 0);
  }
}

static VALUE
rb_jit_start(RB_UNUSED_VAR(VALUE self))
{
  struct sigaction sa;
  struct itimerval timer;

  if (llrb_profiler.running) return Qfalse;
  if (!llrb_profiler.sample_by_frame) {
    llrb_profiler.sample_by_frame = st_init_numtable();
  }

  sa.sa_sigaction = llrb_signal_handler;
  sa.sa_flags = SA_RESTART | SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPROF, &sa, NULL);

  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = 1000;
  timer.it_value = timer.it_interval;
  setitimer(ITIMER_PROF, &timer, 0);

  llrb_profiler.running = 1;
  return Qtrue;
}

static VALUE
rb_jit_stop(RB_UNUSED_VAR(VALUE self))
{
  struct sigaction sa;
  struct itimerval timer;

  if (!llrb_profiler.running) return Qfalse;
  llrb_profiler.running = 0;

	memset(&timer, 0, sizeof(timer));
	setitimer(ITIMER_PROF, &timer, 0);

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPROF, &sa, NULL);

  return Qtrue;
}

static int
llrb_gc_mark_i(st_data_t key, st_data_t val, st_data_t arg)
{
  VALUE frame = (VALUE)key;
  rb_gc_mark(frame);
  return ST_CONTINUE;
}

static void
llrb_gc_mark(void *data)
{
  if (llrb_profiler.sample_by_frame) {
    st_foreach(llrb_profiler.sample_by_frame, llrb_gc_mark_i, 0);
  }
}

static void
llrb_atfork_prepare(void)
{
  struct itimerval timer;
  if (llrb_profiler.running) {
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_PROF, &timer, 0);
  }
}

static void
llrb_atfork_parent(void)
{
  struct itimerval timer;
  if (llrb_profiler.running) {
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 1000;
    timer.it_value = timer.it_interval;
    setitimer(ITIMER_PROF, &timer, 0);
  }
}

static void
llrb_atfork_child(void)
{
  rb_jit_stop(Qnil);
}

void
Init_profiler(VALUE rb_mJIT)
{
  rb_define_singleton_method(rb_mJIT, "start", RUBY_METHOD_FUNC(rb_jit_start), 0);
  rb_define_singleton_method(rb_mJIT, "stop", RUBY_METHOD_FUNC(rb_jit_stop), 0);

  llrb_profiler.running = 0;
  llrb_profiler.profile_times = 0;
  llrb_profiler.sample_by_frame = 0;

  gc_hook = Data_Wrap_Struct(rb_cObject, llrb_gc_mark, NULL, &llrb_profiler);
  rb_global_variable(&gc_hook);

  pthread_atfork(llrb_atfork_prepare, llrb_atfork_parent, llrb_atfork_child);
}
