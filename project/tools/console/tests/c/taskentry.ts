/**
 * @file  tools/console/tests/c/taskentry.ts
 * @brief 给 esbuild 打包用的入口：只做 re-export，让 node 能直接跑上位机的任务编码器。
 *
 * 由 `tools/console/tests/test_task_firmware_e2e.py` 打包成 .mjs 后交给 `emit_tasks.mjs`。
 * 这里**不许有逻辑** —— 只有一个目的：证明"上车的那份字节"就是上位机这份代码编出来的。
 */
export { COND, MODE, NO_STATE, OP, blk, crc16Ccitt, decodeTask, encodeTask, validateTask } from '../../web/src/lib/task';
export { EDIT_MAX_TASKS, defaultTaskFile, newTaskDef, truncateNameUtf8 } from '../../web/src/lib/taskedit';
