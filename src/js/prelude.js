// Copyright 2026 Ian Karlsson
// The parts of the window written in JavaScript, run once when quickdom is installed in a context: what a host
// may already give (console) is left as it is.
(() => {
	const g = globalThis;
	if (!g.console) {
		const text = (x) => (typeof x === 'string' ? x : x instanceof Error ? `${x}\n${x.stack || ''}` : (() => {
			try { return JSON.stringify(x); } catch { return String(x); }
		})());
		const say = (...a) => __qdom.log(a.map(text).join(' '));
		g.console = { log: say, info: say, warn: say, error: say, debug: say, trace: say };
	}
	// what a drag from another program carries, as a drop's dataTransfer: its files, each with its path (as Electron's
	// File has it), known only once it is dropped; while it is dragged over, only that it carries files
	g.__qdom.transfer = (paths, files) => {
		const list = paths.map((path) => ({ name: path.slice(path.lastIndexOf('/') + 1), path, type: '', size: 0 }));
		const uris = paths.map((p) => `file://${encodeURI(p)}`).join('\r\n');
		return {
			dropEffect: 'copy', effectAllowed: 'copy',
			types: files ? ['Files', 'text/uri-list'] : [],
			files: list,
			items: list.map((f) => ({ kind: 'file', type: '', getAsFile: () => f })),
			getData: (type) => (type === 'text/uri-list' || type === 'URL' ? uris : ''),
			setData() {}, clearData() {}, setDragImage() {},
		};
	};
	if (!g.navigator)
		g.navigator = { userAgent: 'quickdom', platform: '', language: 'en-US', languages: ['en-US'] };
	// the host's clipboard (the window's: the system's), as text
	if (!g.navigator.clipboard) {
		const waiting = [];
		g.__qdom.pasted = (text) => {
			for (const w of waiting.splice(0))
				w.resolve(text);
		};
		g.navigator.clipboard = {
			writeText(text) {
				__qdom.clipboardWrite(String(text));
				return Promise.resolve();
			},
			readText() {
				return new Promise((resolve, reject) => {
					waiting.push({ resolve, reject });
					if (waiting.length === 1 && !__qdom.clipboardRead())
						for (const w of waiting.splice(0))
							w.reject(new Error('the host has no clipboard'));
				});
			},
		};
	}
})();
