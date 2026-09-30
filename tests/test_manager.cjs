/* Unified host-test entry point for Manager-side Vela modules. */
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const tests = [
  'test_file.cjs',
  'test_interconnect.cjs',
  'test_resource_storage.cjs',
  'test_reload_signal.cjs',
  'test_resource_order.cjs',
  'test_resource_overrides.cjs',
  'test_manager_layout.cjs'
];

for (const test of tests) {
  const result = spawnSync(process.execPath, [path.join(__dirname, test)], {
    stdio: 'inherit',
    cwd: path.resolve(__dirname, '..')
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status === null ? 1 : result.status);
}

console.log('All Manager host tests passed.');
