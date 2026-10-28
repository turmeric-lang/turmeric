// editor-ref.js -- the live Monaco namespace and editor, for the modules split
// out of main.js.
//
// main.js owns both and creates them during startup, after every module has
// been imported. So it hands this module two getters once, before anything
// else runs, and the other modules read through them: a module that imported
// the variables instead would hold their value at import time -- null.

let editorGetter = () => null;
let monacoGetter = () => null;

/** Called once by main.js with getters over its own `editor` and `monaco`. */
export function provideEditor(getEditor, getMonaco) {
    editorGetter = getEditor;
    monacoGetter = getMonaco;
}

/** The editor instance, or null before it exists. */
export function getEditor() {
    return editorGetter();
}

/** The Monaco namespace, or null before it has loaded. */
export function getMonaco() {
    return monacoGetter();
}
