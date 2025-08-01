#!/usr/bin/env bash

# [[ $VERBOSE == 1 ]] && set -x

PROGNAME="${BASH_SOURCE[0]}"
HERE="$(cd "$(dirname "$PROGNAME")" &>/dev/null && pwd)"
ROOT=$(cd $HERE/../.. && pwd)

export PYTHONUNBUFFERED=1

VG_REDIS_VER=7.4
VG_REDIS_SUFFIX=7.4
SAN_REDIS_VER=7.4
SAN_REDIS_SUFFIX=7.4

cd $HERE

#----------------------------------------------------------------------------------------------

help() {
	cat <<-'END'
		Run Python tests using RLTest

		[ARGVARS...] runtests.sh [--help|help] [<module-so-path>] [extra RLTest args...]

		Argument variables:
		MODULE=path           Path to RediSearch module .so
		MODARGS=args          RediSearch module arguments
		BINROOT=path          Path to repo binary root dir

		REDIS_STANDALONE=1|0  Test with standalone Redis (default: 1)
		SA=1|0                Alias for REDIS_STANDALONE
		SHARDS=n              Number of OSS coordinator shards (default: 3)
		QUICK=1|~1|0          Perform only common test variant (~1: all but common)
		CONFIG=cfg            Perform one of: raw_docid, dialect_2,

		TEST=name             Run specific test (e.g. test.py:test_name)
		TESTFILE=file         Run tests listed in `file`
		FAILEDFILE=file       Write failed tests into `file`

		UNSTABLE=1            Do not skip unstable tests (default: 0)
		ONLY_STABLE=1         Skip unstable tests
		REJSON=1|0            Also load RedisJSON module (default: 1)
		REJSON_BRANCH=branch  Use RedisJSON module from branch (default: 'master')
		REJSON_PATH=path      Use RedisJSON module at `path` (default: '' - build from source)
		REJSON_ARGS=args      RedisJSON module arguments

		REDIS_SERVER=path     Location of redis-server
		REDIS_PORT=n          Redis server port
		CONFIG_FILE=file      Path to config file

		EXT=1|run             Test on existing env (1=running; run=start redis-server)
		EXT_HOST=addr         Address of existing env (default: 127.0.0.1)
		EXT_PORT=n            Port of existing env

		RLEC=0|1              General tests on RLEC
		DOCKER_HOST=addr      Address of Docker server (default: localhost)
		RLEC_PORT=port        Port of RLEC database (default: 12000)

		COV=1                 Run with coverage analysis
		VG=1                  Run with Valgrind
		VG_LEAKS=0            Do not detect leaks
		SAN=type              Use LLVM sanitizer (type=address|memory|leak|thread)
		BB=1                  Enable Python debugger (break using BB() in tests)
		GDB=1                 Enable interactive gdb debugging (in single-test mode)

		RLTEST=path|'view'    Take RLTest from repo path or from local view
		RLTEST_DEBUG=1        Show debugging printouts from tests
		RLTEST_ARGS=args      Extra RLTest args
		LOG_LEVEL=<level>     Set log level (default: debug)
		TEST_TIMEOUT=n        Set RLTest test timeout in seconds (default: 300)

		PARALLEL=1            Runs tests in parallel
		SLOW=1                Do not test in parallel
		UNIX=1                Use unix sockets
		RANDPORTS=1           Use randomized ports

		CLEAR_LOGS=0          Do not remove logs prior to running tests

		LIST=1                List all tests and exit
		ENV_ONLY=1            Just start environment, run no tests
		VERBOSE=1             Print commands and Redis output
		LOG=1                 Send results to log (even on single-test mode)
		KEEP=1                Do not remove intermediate files
		NOP=1                 Dry run
		HELP=1                Show help

	END
}


setup_rltest() {
	if [[ $RLTEST == view ]]; then
		if [[ ! -d $ROOT/../RLTest ]]; then
			eprint "RLTest not found in view $ROOT"
			exit 1
		fi
		RLTEST=$(cd $ROOT/../RLTest; pwd)
	fi

	if [[ -n $RLTEST ]]; then
		if [[ ! -d $RLTEST ]]; then
			eprint "Invalid RLTest location: $RLTEST"
			exit 1
		fi

		# Specifically search for it in the specified location
		export PYTHONPATH="$PYTHONPATH:$RLTEST"
		if [[ $VERBOSE == 1 ]]; then
			echo "PYTHONPATH=$PYTHONPATH"
		fi
	fi

	RLTEST_ARGS+=" --allow-unsafe"  # allow redis use debug and module command and change protected configs

	LOG_LEVEL=${LOG_LEVEL:-debug}
	RLTEST_ARGS+=" --log-level $LOG_LEVEL"

	TEST_TIMEOUT=${TEST_TIMEOUT:-300}
	RLTEST_ARGS+=" --test-timeout $TEST_TIMEOUT"

	if [[ $RLTEST_VERBOSE == 1 ]]; then
		RLTEST_ARGS+=" -v"
	fi
	if [[ $RLTEST_DEBUG == 1 ]]; then
		RLTEST_ARGS+=" --debug-print"
	fi
	if [[ -n $RLTEST_LOG && $RLTEST_LOG != 1 && -z $RLTEST_PARALLEL_ARG ]]; then
		RLTEST_ARGS+=" -s"
	fi
	if [[ $RLTEST_CONSOLE == 1 ]]; then
		RLTEST_ARGS+=" -i"
	fi
}

#----------------------------------------------------------------------------------------------

setup_clang_sanitizer() {
	local ignorelist=$ROOT/tests/memcheck/redis.san-ignorelist
	if ! grep THPIsEnabled $ignorelist &> /dev/null; then
		echo "fun:THPIsEnabled" >> $ignorelist
	fi

	# for RediSearch module
	export RS_GLOBAL_DTORS=1

	# for RLTest
	export SANITIZER="$SAN"
	export SHORT_READ_BYTES_DELTA=512

	# --no-output-catch --exit-on-failure --check-exitcode
	RLTEST_SAN_ARGS="--sanitizer $SAN"
	if [[ $SAN == addr || $SAN == address ]]; then
		# RLTest places log file details in ASAN_OPTIONS
		export ASAN_OPTIONS="detect_odr_violation=0:halt_on_error=0:detect_leaks=1:verbosity=0"
		export LSAN_OPTIONS="suppressions=$ROOT/tests/memcheck/asan.supp:print_suppressions=0:verbosity=0"
		# :use_tls=0

	fi
}

#----------------------------------------------------------------------------------------------

setup_redis_server() {
	REDIS_SERVER=${REDIS_SERVER:-redis-server}

	if ! command -v $REDIS_SERVER &> /dev/null; then
		echo "Cannot find $REDIS_SERVER. Aborting."
		exit 1
	fi
}


#----------------------------------------------------------------------------------------------

setup_coverage() {
	# RLTEST_COV_ARGS="--unix"

	export CODE_COVERAGE=1
	export RS_GLOBAL_DTORS=1
}

#----------------------------------------------------------------------------------------------

run_env() {
	if [[ $REDIS_STANDALONE == 0 ]]; then
		oss_cluster_args="--env oss-cluster --shards-count $SHARDS"
		RLTEST_ARGS+=" ${oss_cluster_args}"
	fi

	rltest_config=$(mktemp "${TMPDIR:-/tmp}/rltest.XXXXXXX")
	rm -f $rltest_config
	cat <<-EOF > $rltest_config
		--env-only
		--oss-redis-path=$REDIS_SERVER
		--module $MODULE
		--module-args '$MODARGS'
		$RLTEST_ARGS
		$RLTEST_TEST_ARGS
		$RLTEST_PARALLEL_ARG
		$RLTEST_REJSON_ARGS
		$RLTEST_VG_ARGS
		$RLTEST_SAN_ARGS
		$RLTEST_COV_ARGS

		EOF

	# Use configuration file in the current directory if it exists
	if [[ -n $CONFIG_FILE && -e $CONFIG_FILE ]]; then
		cat $CONFIG_FILE >> $rltest_config
	fi

	if [[ $VERBOSE == 1 || $NOP == 1 ]]; then
		echo "RLTest configuration:"
		cat $rltest_config
		[[ -n $VG_OPTIONS ]] && { echo "VG_OPTIONS: $VG_OPTIONS"; echo; }
	fi

	local E=0
	if [[ $NOP != 1 ]]; then
		{ $OP python3 -m RLTest @$rltest_config; (( E |= $? )); } || true
	else
		$OP python3 -m RLTest @$rltest_config
	fi

	[[ $KEEP != 1 ]] && rm -f $rltest_config

	return $E
}

#----------------------------------------------------------------------------------------------

run_tests() {
	local title="$1"
	shift
	if [[ -n $title ]]; then
		if [[ -n $GITHUB_ACTIONS ]]; then
			echo "::group::$title"
		else
			printf "Running $title:\n\n"
		fi
	fi
	# TODO:Remove this once RLTest progress bar is fixed
	RLTEST_ARGS+=" --no-progress"

	if [[ $EXT != 1 ]]; then
		rltest_config=$(mktemp "${TMPDIR:-/tmp}/rltest.XXXXXXX")
		rm -f $rltest_config
		if [[ $RLEC != 1 ]]; then
			cat <<-EOF > $rltest_config
				--oss-redis-path=$REDIS_SERVER
				--module $MODULE
				--module-args '$MODARGS'
				$RLTEST_ARGS
				$RLTEST_TEST_ARGS
				$RLTEST_PARALLEL_ARG
				$RLTEST_REJSON_ARGS
				$RLTEST_VG_ARGS
				$RLTEST_SAN_ARGS
				$RLTEST_COV_ARGS

				EOF
		else
			cat <<-EOF > $rltest_config
				$RLTEST_ARGS
				$RLTEST_TEST_ARGS
				$RLTEST_VG_ARGS

				EOF
		fi
	else # existing env
		if [[ $EXT == run ]]; then
			if [[ $REJSON_MODULE ]]; then
				XREDIS_REJSON_ARGS="loadmodule $REJSON_MODULE $REJSON_ARGS"
			fi

			xredis_conf=$(mktemp "${TMPDIR:-/tmp}/xredis_conf.XXXXXXX")
			rm -f $xredis_conf
			cat <<-EOF > $xredis_conf
				loadmodule $MODULE $MODARGS
				$XREDIS_REJSON_ARGS
				EOF

			rltest_config=$(mktemp "${TMPDIR:-/tmp}/xredis_rltest.XXXXXXX")
			rm -f $rltest_config
			cat <<-EOF > $rltest_config
				--env existing-env
				$RLTEST_ARGS
				$RLTEST_TEST_ARGS

				EOF

			if [[ $VERBOSE == 1 ]]; then
				echo "External redis-server configuration:"
				cat $xredis_conf
			fi

			$REDIS_SERVER $xredis_conf &
			XREDIS_PID=$!
			echo "External redis-server pid: " $XREDIS_PID

		else # EXT=1
			rltest_config=$(mktemp "${TMPDIR:-/tmp}/xredis_rltest.XXXXXXX")
			[[ $KEEP != 1 ]] && rm -f $rltest_config
			cat <<-EOF > $rltest_config
				--env existing-env
				--existing-env-addr $EXT_HOST:$EXT_PORT
				$RLTEST_ARGS
				$RLTEST_TEST_ARGS

				EOF
		fi
	fi

	# Use configuration file in the current directory if it exists
	if [[ -n $CONFIG_FILE && -e $CONFIG_FILE ]]; then
		cat $CONFIG_FILE >> $rltest_config
	fi

	if [[ $VERBOSE == 1 || $NOP == 1 ]]; then
		echo "RLTest configuration:"
		cat $rltest_config
		[[ -n $VG_OPTIONS ]] && { echo "VG_OPTIONS: $VG_OPTIONS"; echo; }
	fi

	[[ $RLEC == 1 ]] && export RLEC_CLUSTER=1

	local E=0
	if [[ $NOP != 1 ]]; then
		{ $OP python3 -m RLTest @$rltest_config; (( E |= $? )); } || true
	else
		$OP python3 -m RLTest @$rltest_config
	fi

	[[ $KEEP != 1 ]] && rm -f $rltest_config

	if [[ -n $XREDIS_PID ]]; then
		echo "killing external redis-server: $XREDIS_PID"
		kill -TERM $XREDIS_PID
	fi

	if [[ -n $GITHUB_ACTIONS ]]; then
		echo "::endgroup::"
		if [[ $E != 0 ]]; then
			echo "::error:: $title failed, code: $E"
		fi
	fi
	return $E
}

#------------------------------------------------------------------------------------ Arguments

if [[ $1 == --help || $1 == help || $HELP == 1 ]]; then
	help
	exit 0
fi

OP=""
[[ $NOP == 1 ]] && OP=echo

#--------------------------------------------------------------------------------- Environments

DOCKER_HOST=${DOCKER_HOST:-127.0.0.1}
RLEC_PORT=${RLEC_PORT:-12000}

EXT_HOST=${EXT_HOST:-127.0.0.1}
EXT_PORT=${EXT_PORT:-6379}

PID=$$


# RLTest uses `fork` which might fail on macOS with the following variable set
[[ $OS == macos ]] && export OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES

#---------------------------------------------------------------------------------- Tests scope

# Fallback: REDIS_STANDALONE -> SA -> 1
REDIS_STANDALONE=${REDIS_STANDALONE:-$SA}
REDIS_STANDALONE=${REDIS_STANDALONE:-1}

RLEC=${RLEC:-0}

if [[ $RLEC != 1 ]]; then
	MODULE="${MODULE:-$1}"
	shift

	if [[ -z $MODULE ]]; then
		if [[ -n $BINROOT ]]; then
			# By default, we test the module with the coordinator (for both cluster and standalone)
			MODULE=$BINROOT/coord-oss/redisearch.so
		fi
		if [[ -z $MODULE || ! -f $MODULE ]]; then
			echo "Module not found at ${MODULE}. Aborting."
			exit 1
		fi
	fi
else
	REDIS_STANDALONE= # RLEC and REDIS_STANDALONE are mutually exclusive
fi

SHARDS=${SHARDS:-3}

#------------------------------------------------------------------------------------ Debugging

VG_LEAKS=${VG_LEAKS:-1}
VG_ACCESS=${VG_ACCESS:-1}

GDB=${GDB:-0}

if [[ $GDB == 1 ]]; then
	[[ $LOG != 1 ]] && RLTEST_LOG=0
	RLTEST_CONSOLE=1
fi

[[ $SAN == addr ]] && SAN=address
[[ $SAN == mem ]] && SAN=memory

if [[ -n $TEST ]]; then
	[[ $LOG != 1 ]] && RLTEST_LOG=0
	# export BB=${BB:-1}
	export RUST_BACKTRACE=1
fi


#---------------------------------------------------------------------------------- Parallelism

PARALLEL=${PARALLEL:-1}

[[ $EXT == 1 || $EXT == run || $BB == 1 || $GDB == 1 ]] && PARALLEL=0

if [[ -n $PARALLEL && $PARALLEL != 0 ]]; then
	if [[ $PARALLEL == 1 ]]; then
		parallel="$(nproc)"
	else
		parallel=$PARALLEL
	fi
	if (( $parallel==0 )) ; then parallel=1 ; fi
	RLTEST_PARALLEL_ARG="--parallelism $parallel"
fi
#------------------------------------------------------------------------------- Test selection

if [[ -n $TEST ]]; then
	RLTEST_TEST_ARGS+=$(echo -n " "; echo "$TEST" | awk 'BEGIN { RS=" "; ORS=" " } { print "--test " $1 }')
fi

if [[ -n $TESTFILE ]]; then
	if ! is_abspath "$TESTFILE"; then
		TESTFILE="$ROOT/$TESTFILE"
	fi
	RLTEST_TEST_ARGS+=" -f $TESTFILE"
fi

if [[ -n $FAILEDFILE ]]; then
	if ! is_abspath "$FAILEDFILE"; then
		TESTFILE="$ROOT/$FAILEDFILE"
	fi
	RLTEST_TEST_ARGS+=" -F $FAILEDFILE"
fi

if [[ $LIST == 1 ]]; then
	NO_SUMMARY=1
	RLTEST_ARGS+=" --collect-only"
fi

#---------------------------------------------------------------------------------------- Setup

if [[ $VERBOSE == 1 ]]; then
	RLTEST_VERBOSE=1
fi

RLTEST_LOG=${RLTEST_LOG:-$LOG}

if [[ $COV == 1 ]]; then
	setup_coverage
fi

# Prepare RedisJSON module to be loaded into testing environment if required.
if [[ $REJSON != 0 ]]; then
  ROOT="$ROOT" BINROOT="${BINROOT}/${FULL_VARIANT}" REJSON_BRANCH="$REJSON_BRANCH" source $ROOT/tests/deps/setup_rejson.sh
  echo "Using RedisJSON module at $JSON_BIN_PATH, with the following args: $REJSON_ARGS"
  RLTEST_REJSON_ARGS="--module ${JSON_BIN_PATH} --module-args $REJSON_ARGS"
else
  echo "Skipping tests with RedisJSON module"
fi

RLTEST_ARGS+=" $@"

if [[ -n $REDIS_PORT ]]; then
	RLTEST_ARGS+="--redis-port $REDIS_PORT"
fi

[[ $UNIX == 1 ]] && RLTEST_ARGS+=" --unix"
[[ $RANDPORTS == 1 ]] && RLTEST_ARGS+=" --randomize-ports"

#----------------------------------------------------------------------------------------------

setup_rltest

if [[ -n $SAN ]]; then
	setup_clang_sanitizer
fi

if [[ $RLEC != 1 ]]; then
	setup_redis_server
fi

#------------------------------------------------------------------------------------- Env only

if [[ $ENV_ONLY == 1 ]]; then
	run_env
	exit 0
fi

#-------------------------------------------------------------------------------- Running tests

if [[ $CLEAR_LOGS != 0 ]]; then
	rm -rf $HERE/logs
fi

E=0

MODARGS="${MODARGS}; TIMEOUT 0;" # disable query timeout by default

if [[ $GC == 0 ]]; then
	MODARGS="${MODARGS}; NOGC;"
fi

echo "Running tests in parallel using $parallel Python processes"

if [[ $REDIS_STANDALONE == 1 ]]; then
	if [[ $QUICK != "~1" && -z $CONFIG ]]; then
		{ (run_tests "RediSearch tests"); (( E |= $? )); } || true
	fi

	if [[ $QUICK != 1 ]]; then

		if [[ -z $CONFIG || $CONFIG == raw_docid ]]; then
			{ (MODARGS="${MODARGS}; RAW_DOCID_ENCODING true;" \
				run_tests "with raw DocID encoding"); (( E |= $? )); } || true
		fi

		if [[ -z $CONFIG || $CONFIG == dialect_2 ]]; then
			{ (MODARGS="${MODARGS}; DEFAULT_DIALECT 2;" \
				run_tests "with Dialect v2"); (( E |= $? )); } || true
		fi
	fi

elif [[ $REDIS_STANDALONE == 0 ]]; then
	oss_cluster_args="--env oss-cluster --shards-count $SHARDS"

	# Increase timeout (to 5 min) for tests with coordinator to avoid cluster fail when it take more time for
	# passing PINGs between shards
  	oss_cluster_args="${oss_cluster_args} --cluster_node_timeout 300000"

	{ (MODARGS="${MODARGS}" RLTEST_ARGS="$RLTEST_ARGS ${oss_cluster_args}" \
	   run_tests "OSS cluster tests"); (( E |= $? )); } || true
fi

if [[ $RLEC == 1 ]]; then
	dhost=$(echo "$DOCKER_HOST" | awk -F[/:] '{print $4}')
	{ (RLTEST_ARGS+="${RLTEST_ARGS} --env existing-env --existing-env-addr $dhost:$RLEC_PORT" \
	   run_tests "tests on RLEC"); (( E |= $? )); } || true
fi

#-------------------------------------------------------------------------------------- Summary

if [[ $NO_SUMMARY == 1 ]]; then
	exit 0
fi

if [[ $NOP != 1 ]]; then
	if [[ -n $SAN || $VG == 1 ]]; then
		{ FLOW=1 $ROOT/sbin/memcheck-summary; (( E |= $? )); } || true
	fi
fi


exit $E
