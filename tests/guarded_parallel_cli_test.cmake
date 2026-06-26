execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" -c
    "import ast, pathlib; ast.parse(pathlib.Path(r'${GUARDED_PARALLEL}').read_text(encoding='utf-8'))"
  RESULT_VARIABLE py_compile_result
  OUTPUT_VARIABLE py_compile_stdout
  ERROR_VARIABLE py_compile_stderr
)
if(NOT py_compile_result EQUAL 0)
  message(FATAL_ERROR
    "guarded_parallel.py must compile\n${py_compile_stdout}\n${py_compile_stderr}")
endif()

execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" "${GUARDED_PARALLEL}"
    --print-budget
    --json
    --max-jobs 4
    --per-job-mb 2048
    --min-free-mb 4096
  RESULT_VARIABLE budget_result
  OUTPUT_VARIABLE budget_stdout
  ERROR_VARIABLE budget_stderr
)
if(NOT budget_result EQUAL 0)
  message(FATAL_ERROR
    "guarded_parallel.py --print-budget must succeed\n${budget_stdout}\n${budget_stderr}")
endif()
if(NOT budget_stdout MATCHES "\"recommended_jobs\"")
  message(FATAL_ERROR
    "guarded_parallel.py budget output must include recommended_jobs\n${budget_stdout}")
endif()

execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" "${GUARDED_PARALLEL}"
    --print-budget
    --json
    --max-jobs 4
    --per-job-mb 2048
    --min-free-mb 999999999
  RESULT_VARIABLE zero_budget_result
  OUTPUT_VARIABLE zero_budget_stdout
  ERROR_VARIABLE zero_budget_stderr
)
if(NOT zero_budget_result EQUAL 0)
  message(FATAL_ERROR
    "guarded_parallel.py zero-budget print must succeed\n${zero_budget_stdout}\n${zero_budget_stderr}")
endif()
if(NOT zero_budget_stdout MATCHES "\"recommended_jobs\": 0")
  message(FATAL_ERROR
    "guarded_parallel.py must fail closed to recommended_jobs=0 under impossible reserve\n${zero_budget_stdout}")
endif()

execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" "${GUARDED_PARALLEL}"
    --max-jobs 1
    --per-job-mb 1
    --min-free-mb 999999999
    --job "${Python3_EXECUTABLE} --version"
  RESULT_VARIABLE zero_budget_job_result
  OUTPUT_VARIABLE zero_budget_job_stdout
  ERROR_VARIABLE zero_budget_job_stderr
)
if(zero_budget_job_result EQUAL 0)
  message(FATAL_ERROR
    "guarded_parallel.py must fail closed instead of waiting forever when jobs cannot start\n${zero_budget_job_stdout}\n${zero_budget_job_stderr}")
endif()
if(NOT zero_budget_job_stderr MATCHES "insufficient_memory")
  message(FATAL_ERROR
    "guarded_parallel.py zero-budget job failure must explain insufficient_memory\n${zero_budget_job_stdout}\n${zero_budget_job_stderr}")
endif()
if(zero_budget_job_stdout MATCHES "start job")
  message(FATAL_ERROR
    "guarded_parallel.py zero-budget job must not start any job\n${zero_budget_job_stdout}")
endif()

execute_process(
  COMMAND
    "${Python3_EXECUTABLE}" "${GUARDED_PARALLEL}"
    --max-jobs 1
    --per-job-mb 1
    --min-free-mb 0
    --stop-on-failure
    --job "${Python3_EXECUTABLE} --version"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_stdout
  ERROR_VARIABLE run_stderr
)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR
    "guarded_parallel.py must run a simple guarded job\n${run_stdout}\n${run_stderr}")
endif()
