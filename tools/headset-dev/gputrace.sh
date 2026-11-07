#!/bin/sh
# gputrace.sh [seconds]: GPU command batches per context (adreno tracepoints) -> /tmp/gpu.trace
T=/sys/kernel/debug/tracing
echo 0 > $T/tracing_on; echo > $T/trace; echo 8192 > $T/buffer_size_kb
for e in adreno_cmdbatch_submitted adreno_cmdbatch_retired adreno_cmdbatch_queued adreno_preempt_trigger adreno_preempt_done; do echo 1 > $T/events/kgsl/$e/enable; done
echo 1 > $T/tracing_on; sleep ${1:-1}; echo 0 > $T/tracing_on
cat $T/trace > /tmp/gpu.trace
for e in adreno_cmdbatch_submitted adreno_cmdbatch_retired adreno_cmdbatch_queued adreno_preempt_trigger adreno_preempt_done; do echo 0 > $T/events/kgsl/$e/enable; done
wc -l /tmp/gpu.trace
