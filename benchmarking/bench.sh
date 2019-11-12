#!/bin/sh

# Parameters: <NF directory> <bench type (latency/throughput)> <layer of flows in bench>
# Overrideable variables:
# - NF_NAME, defaults to 'tinynf', the process name of the NF (used to check if it's alive and kill it later)
# Overrideable behavior:
# - By default, after compiling with make, runs ./$NF_NAME; override this by providing a 'run' target in the makefile in which case arguments are passed as the TN_ARGS variable

if [ -z "$NF_NAME" ]; then
  NF_NAME='tinynf'
fi
LOG_FILE='bench.log'
RESULTS_FILE='bench.results'

if [ -z "$1" ]; then
  echo "[ERROR] Please provide the directory of the NF as the first argument to $0"
  exit 1
fi
NF_DIR="$1"

if [ -z "$2" ]; then
  echo "[ERROR] Please provide the type of benchmark as the second argument to $0"
  exit 1
fi
BENCH_TYPE="$2"

if [ -z "$3" ]; then
  echo "[ERROR] Please provide the layer of the benchmark as the third argument to $0"
  exit 1
fi
BENCH_LAYER="$3"

if [ ! -z "$(pgrep "$NF_NAME")" ]; then
  echo '[ERROR] The NF is already running'
  exit 1
fi

if [ ! -f config ]; then
  echo "[ERROR] Please create a 'config' file from the 'config.template' file in the same directory as $0"
  exit 1
fi
. ./config

echo '[bench] Cloning submodules...'
git submodule update --init --recursive

echo '[bench] Copying scripts on tester...'
rsync -a -q --exclude '*.log' --exclude '*.results' . "$TESTER_HOST:tinynf-benchmarking"

echo '[bench] Building NF...'
make -C "$NF_DIR" >/dev/null

echo '[bench] Running NF...'
make -C "$NF_DIR" -q run >/dev/null 2>&1
if [ $? -eq 2 ]; then  # Exit code 2 means the target does not exist
  sudo taskset -c "$MB_CPU" "$NF_DIR"/"$NF_NAME" "$MB_DEV_0" "$MB_DEV_1" >"$LOG_FILE" 2>&1 &
else
  TN_ARGS="$MB_DEV_0 $MB_DEV_1" taskset -c "$MB_CPU" make -C "$NF_DIR" run >"$LOG_FILE" 2>&1 &
fi
sleep 1 # so that the NF has time to fail if needed
NF_PID=$(pgrep "$NF_NAME")
if [ -z "$NF_PID" ]; then
  echo "[ERROR] Could not launch the NF. The $LOG_FILE file in the same directory as $0 may be useful"
  exit 1
fi

echo '[bench] Running benchmark on tester...'
ssh "$TESTER_HOST" "cd tinynf-benchmarking; ./bench-tester.sh $BENCH_TYPE $BENCH_LAYER"

echo '[bench] Fetching results...'
scp "$TESTER_HOST:tinynf-benchmarking/results.csv" "$RESULTS_FILE"

echo '[bench] Stopping NF...'
sudo kill -9 "$NF_PID" >/dev/null 2>&1

echo "[bench] Done! Results are in $RESULTS_FILE, and the log in $LOG_FILE, in the same directory as $0"