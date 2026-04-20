### Post-completion checklist (applies to ALL tasks)

After completing the task deliverables:

1. **Run the full test suite:**
   ```bash
   cd packages/emu86
   make test-unit
   ```

2. **Update the task log** — append to `tasks/completed/task-log.md`:
   ```
   ## {TASK-ID}
   Date: {today's date}
   Status: PASS / FAIL
   Test results: {X passed, Y failed}
   Notes: {any issues encountered, design decisions made, or deviations from the task spec}
   ```

3. **If all tests pass:**
   ```bash
   cd ../../                          # repo root
   mv tasks/{task-file} tasks/completed/
   git add -A
   git commit -m "{TASK-ID}: {brief description}"
   git push origin master
   ```

4. **If any tests fail:**
   - Fix the issues and re-run tests
   - If you cannot resolve a failure, document it in the task log with `Status: PARTIAL` and details of what failed and why
   - Still commit and push, but do NOT move the task file to completed/
   - The commit message should be: "{TASK-ID}: {brief description} (PARTIAL - see task log)"
