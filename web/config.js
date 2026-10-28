// config.js -- the playground's settings.

export const CONFIG = {
    DEFAULT_CODE: `#lang turmeric/sweet

println "Hello, Turmeric!"
`,
    // How long one request may keep the eval Worker busy before the watchdog
    // stops it and starts a fresh session (security-audit-plan W-3). Generous,
    // because a playground program that legitimately computes for a while is
    // not the problem; a program that never returns is.
    EXECUTION_TIMEOUT: 30000,
    MAX_OUTPUT_LENGTH: 10000,
};
