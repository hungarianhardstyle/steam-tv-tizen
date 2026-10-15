const { spawnSync } = require('node:child_process');
const result = spawnSync(process.execPath, [require('node:path').join(__dirname, 'test-native.js')], { stdio: 'inherit', timeout: 15000 });
if (result.error) console.error(result.error.message);
process.exitCode = result.status === 0 && !result.error ? 0 : 1;
