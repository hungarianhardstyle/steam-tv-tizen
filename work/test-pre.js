// Samsung's host-binding lookup expects the browser global even in Node test runs.
if (typeof self === 'undefined') global.self = global;
// Node's importScripts emulation does not retain the SDK's top-level let binding.
if (typeof global !== 'undefined') global.USE_HOST_BINDINGS = false;
