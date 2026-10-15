// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 GhostLock-X200-App contributors
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const folder = path.join(__dirname, '../app/src/main/assets/www/assets');
const file = fs.readdirSync(folder).find(name => name.endsWith('.js'));
const source = fs.readFileSync(path.join(folder, file), 'utf8');
const start = source.indexOf('let oe=[{re:/STAGE4');
const end = source.indexOf('];function se', start);
assert(start >= 0 && end > start);
const rules = new Function('return ' + source.slice(start + 'let oe='.length, end + 1))();
function classify(line) {
    for (const rule of rules) {
        const match = rule.re.exec(line);
        if (match) return {title: rule.title, text: rule.msg(match)};
    }
    return null;
}
assert.strictEqual(classify('GATE OK: kernel 6.6.89'), null);
assert.strictEqual(classify('GATE FAIL: a manager kernel is already loaded; reboot before changing/loading managers').title, '管理器内核已加载');
assert.strictEqual(classify('GATE FAIL: cannot inspect loaded modules').title, '无法检查已加载模块');
assert.strictEqual(classify('GATE FAIL: cannot read kernel release').title, '无法读取内核版本');
assert.strictEqual(classify("GATE FAIL: kernel release 'other'").title, '内核版本不匹配，已停止');
assert.strictEqual(classify('ROOT_SESSION FAIL: existing manager module has a different or unreadable daemon').title, '已有管理器版本不同');
assert(!classify('GATE FAIL: some other gate').text.includes('无法读取内核版本'));
assert.strictEqual(classify('GL_STATE FAIL: legacy state from this boot; reboot required before retry').title, '运行状态异常，已停止');
assert.strictEqual(classify('GL_STATE FAIL: cannot reserve slot (Permission denied); no glt started').title, '运行状态异常，已停止');
for (const stage of [1, 2, 5]) {
    const result = classify(`STAGE${stage} FAIL: GL_STATE reservation failed; stopped before glt`);
    assert.strictEqual(result.title, '运行状态异常，已停止');
    assert(!result.text.includes('可重试'));
}
for (const line of ['PROCESS_CLEANUP FAIL: worker still active',
    'STAGE1 FAIL: task execution/cleanup failed; stopped',
    'STAGE2 FAIL: task execution/cleanup failed; stopped',
    'STAGE5 FAIL: task execution/cleanup failed; stopped']) {
    assert.strictEqual(classify(line).title, '后台任务未能结束，已停止');
}
const callbackStart = source.indexOf('window.addEventListener(`GhostStage`,(');
const callbackEnd = source.indexOf(')),window.addEventListener(`GhostDone`', callbackStart);
assert(callbackStart >= 0 && callbackEnd > callbackStart);
const callbackSource = source.slice(callbackStart + 'window.addEventListener(`GhostStage`,('.length, callbackEnd);
const progress = {value: -1};
const callback = new Function('r', 'return (' + callbackSource + ')')(progress);
for (let index = 0; index < 9; index++) {
    callback({detail: {index}});
    assert.strictEqual(progress.value, index);
}
callback({detail: {index: 5}});
assert.strictEqual(progress.value, 8);
callback({detail: {index: 9}});
assert.strictEqual(progress.value, 9);
console.log('UI: 16 error classifier cases and 11 progress transitions PASS');
