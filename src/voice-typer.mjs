#!/usr/bin/env node
/**
 * voice-typer —— 本地 SenseVoice 语音输入守护进程 / 客户端
 *
 * 守护进程：
 *   - 一次性加载本机 SenseVoiceSmall + Silero VAD（常驻内存，不再重复加载）
 *   - 从 PulseAudio 录制麦克风（parecord，16kHz 单声道 s16le）
 *   - 实时用 VAD 分段，检测到语音结束后自动停止（免手动）
 *   - 识别文字后用 bin/xtype 直接打进当前焦点窗口
 *
 * 客户端（同一个文件的不同子命令）：
 *   voice-typer dmn            前台运行守护进程
 *   voice-typer toggle         开始/停止（停止后识别并注入）
 *   voice-typer start | stop   显式开始/停止
 *   voice-typer status         查看状态
 *   voice-typer quit           关闭守护进程
 *   voice-typer selftest       只识别一段测试音频并打印，不注入
 *
 * 环境变量：
 *   VOICE_TYPER_SOCK   控制套接字路径（默认 $HOME/.voice-typer.sock）
 *   VOICE_TYPER_LANG   识别语言提示，默认 auto
 */

import net from 'node:net';
import { spawn, execFile } from 'node:child_process';
import { createRequire } from 'node:module';
import { existsSync, unlinkSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import os from 'node:os';

const require = createRequire(import.meta.url);

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);
const XTYPE = join(ROOT, 'bin', 'xtype');
const XPASTE = join(ROOT, 'bin', 'xpaste');
const ASSETS = join(ROOT, 'assets');

const MODELS = process.env.VOICE_TYPER_MODELS
	?? join(os.homedir(), '.dsh', 'speech-to-text', 'sensevoice', 'models');
const MODEL_ONNX = join(MODELS, 'sensevoice-onnx', 'model.int8.onnx');
const TOKENS = join(MODELS, 'sensevoice-onnx', 'tokens.txt');
const VAD_ONNX = join(MODELS, 'silero', 'silero_vad.onnx');

/* 标点模型（CT-Transformer zh-en, int8, 72MB）。VOICE_TYPER_PUNCT=0 可关闭。 */
const PUNCT_ONNX = join(ROOT, 'models', 'punct', 'model.int8.onnx');
const PUNCT_ENABLED = process.env.VOICE_TYPER_PUNCT !== '0';

const SOCK = process.env.VOICE_TYPER_SOCK ?? join(os.homedir(), '.voice-typer.sock');
const LANG_HINT = process.env.VOICE_TYPER_LANG ?? 'auto';

const SAMPLE_RATE = 16000;
const FRAME = 512;

/* 自动停止参数
 * 默认关闭：改为「按两次快捷键」模式——第一次开始，第二次立刻停止并识别。
 * 想恢复「说完自动停」，设 VOICE_TYPER_SILENCE_MS=900 之类即可。 */
const AUTO_STOP_SILENCE_MS = Number(process.env.VOICE_TYPER_SILENCE_MS ?? 0);
const AUTO_STOP = AUTO_STOP_SILENCE_MS > 0;
const MAX_RECORD_SECONDS = Number(process.env.VOICE_TYPER_MAX_SECONDS ?? 120);
const MIN_RECORD_SECONDS = 0.25;
/* 首段前置补多少秒，防止第一个音节被 VAD 切掉 */
const ONSET_PADDING_SECONDS = Number(process.env.VOICE_TYPER_ONSET_PADDING ?? 0.3);

/* 停止录音时的优雅退出等待：越短越快出字 */
const KILL_GRACE_MS = Number(process.env.VOICE_TYPER_KILL_GRACE_MS ?? 250);

const LOG = (...a) => console.error('[voice-typer]', ...a);

/* ---------------- 通知 ---------------- */
/* VOICE_TYPER_NOTIFY=0 可彻底关闭任何弹窗（纯后台） */
const NOTIFY = process.env.VOICE_TYPER_NOTIFY !== '0';
function notify(summary, body = '', urgency = 'normal') {
	if (!NOTIFY) return;
	execFile('notify-send', ['-a', 'voice-typer', '-u', urgency, summary, body], () => {});
}

/* ---------------- 提示音 ---------------- */
const BEEP = process.env.VOICE_TYPER_BEEP !== '0';

/* 提示音：固定绝对响度
 *
 * 目标是不管系统音量滑块在哪，听到的响度都一样；只有系统静音时才不响。
 * 做法是反向补偿 sink 音量：paplay 的流音量上限被硬件层截在 100%，
 * 所以设 stream = target / sink，实际响度 ≈ target。
 *   sink ≥ target → 精确达到目标响度
 *   sink < target → 流音量顶到 100%，只能做到 sink 那么响（物理上限）
 *
 * VOICE_TYPER_BEEP_LEVEL 是目标响度（百分比，相对满幅），默认 11（原先约 22 的一半）。
 */
const BEEP_LEVEL = Math.max(0, Math.min(100, Number(process.env.VOICE_TYPER_BEEP_LEVEL ?? 11)));

/** 读取默认 sink 的静音状态与音量百分比 */
function readSinkState() {
	return new Promise((resolve) => {
		let pending = 2;
		const out = { muted: false, pct: null };
		const done = () => { if (--pending === 0) resolve(out); };
		execFile('pactl', ['get-sink-mute', '@DEFAULT_SINK@'], (err, stdout) => {
			if (!err) out.muted = /yes/i.test(stdout);
			done();
		});
		execFile('pactl', ['get-sink-volume', '@DEFAULT_SINK@'], (err, stdout) => {
			if (!err) {
				const m = String(stdout).match(/(\d+)%/);
				if (m) out.pct = Number(m[1]);
			}
			done();
		});
	});
}

async function beep(which) {
	if (!BEEP) return;
	const st = await readSinkState();
	if (st.muted) return;                       /* 系统静音：不响 */

	let streamPct = 100;
	if (st.pct && st.pct > 0) streamPct = Math.min(100, (BEEP_LEVEL / st.pct) * 100);
	const volume = Math.max(0, Math.min(65536, Math.round(streamPct / 100 * 65536)));

	execFile('paplay', [
		`--volume=${volume}`,
		join(ASSETS, which === 'start' ? 'start.wav' : 'stop.wav')
	], (err) => {
		if (err) LOG('提示音播放失败:', err.message);
	});
}

/* ---------------- 录音指示器 ----------------
 * 用一条「永不自动消失」的高优先级通知作为可见标记（紫红色圆点图标），
 * 停止录音时按 ID 精确关闭它，不会残留。 */
/* 指示器通知的 ID 要落盘。
 * 通知设了 -t 0（永不自动消失），一旦守护进程在录音途中重启，内存里的 ID 就丢了，
 * 那条通知会永久挂在通知中心里——反复重启就会堆成一长串。
 * 因此：ID 写入临时文件；每次显示都用 --replace-id 复用同一个 ID（实测替换后 ID 不变），
 * 保证任何时刻最多只有一条；启动时先清掉上一轮可能遗留的那条。 */
const INDICATOR_ID_FILE = join(os.tmpdir(), 'voice-typer-indicator.id');

function readIndicatorId() {
	try {
		const v = readFileSync(INDICATOR_ID_FILE, 'utf8').trim();
		return /^\d+$/.test(v) ? v : null;
	} catch { return null; }
}
function writeIndicatorId(id) {
	try {
		if (id) writeFileSync(INDICATOR_ID_FILE, String(id), 'utf8');
		else { try { unlinkSync(INDICATOR_ID_FILE); } catch {} }
	} catch {}
}
function closeNotification(id) {
	if (!id) return;
	execFile('gdbus', ['call', '--session',
		'--dest', 'org.freedesktop.Notifications',
		'--object-path', '/org/freedesktop/Notifications',
		'--method', 'org.freedesktop.Notifications.CloseNotification', String(id)], () => {});
}

function indicatorShow() {
	if (!NOTIFY) return;
	const prev = readIndicatorId();
	const msg = ['-a', 'voice-typer', '-u', 'normal', '-t', '0', '-p',
		'-i', 'media-record', '🎤 录音中'];
	if (prev) msg.push('-r', prev);
	execFile('notify-send', msg, (err, stdout) => {
		if (!err) {
			const id = String(stdout).trim();
			if (id) writeIndicatorId(id);
		}
	});
}
function indicatorHide() {
	if (!NOTIFY) return;
	const id = readIndicatorId();
	writeIndicatorId(null);
	closeNotification(id);
}
/** 启动时清理上一轮可能遗留的指示器通知 */
function indicatorCleanupStale() {
	if (!NOTIFY) return;
	const id = readIndicatorId();
	writeIndicatorId(null);
	closeNotification(id);
}

/* ---------------- 识别引擎 ---------------- */
/* 可选模型（用 VOICE_TYPER_MODEL 切换）：
 *   paraformer       232MB，中文、精度更高（默认）
 *   paraformer-small  78MB，中文、速度快
 *   sensevoice       239MB，多语种、原 DSH 模型 */
const MODEL_KIND = process.env.VOICE_TYPER_MODEL ?? 'paraformer';

const MODEL_PRESETS = {
	'sensevoice': {
		dir: MODELS,
		file: MODEL_ONNX,
		tokens: TOKENS,
		build: (p, threads) => ({
			senseVoice: { model: p.file, language: LANG_HINT, useInverseTextNormalization: 1 }
		})
	},
	'paraformer': {
		dir: join(ROOT, 'models', 'paraformer'),
		file: join(ROOT, 'models', 'paraformer', 'model.int8.onnx'),
		tokens: join(ROOT, 'models', 'paraformer', 'tokens.txt'),
		build: (p) => ({ paraformer: { model: p.file } })
	},
	'paraformer-small': {
		dir: join(ROOT, 'models', 'paraformer-small'),
		file: join(ROOT, 'models', 'paraformer-small', 'model.int8.onnx'),
		tokens: join(ROOT, 'models', 'paraformer-small', 'tokens.txt'),
		build: (p) => ({ paraformer: { model: p.file } })
	}
};

function createEngine() {
	const sherpa = require('sherpa-onnx-node');

	const preset = MODEL_PRESETS[MODEL_KIND];
	if (!preset) {
		throw new Error(`未知模型 "${MODEL_KIND}"，可选: ${Object.keys(MODEL_PRESETS).join(' / ')}`);
	}

	for (const [name, p] of [['模型', preset.file], ['词表', preset.tokens], ['VAD', VAD_ONNX]]) {
		if (!existsSync(p)) {
			throw new Error(`找不到${name}文件: ${p}`);
		}
	}

	const cfg = {
		featConfig: { sampleRate: SAMPLE_RATE, featureDim: 80 },
		modelConfig: {
			...preset.build(preset),
			tokens: preset.tokens,
			numThreads: Math.max(1, Math.min(4, os.cpus().length - 1)),
			provider: 'cpu',
			debug: 0
		}
	};
	LOG(`使用模型: ${MODEL_KIND}`);
	const recognizer = new sherpa.OfflineRecognizer(cfg);

	/* 标点后处理：Paraformer 本身不产标点，这里挂 CT-Transformer 补上。
	 * 纯文本推理，5~12ms，与识别质量无关。 */
	let punct = null;
	if (PUNCT_ENABLED) {
		if (existsSync(PUNCT_ONNX)) {
			try {
				punct = new sherpa.OfflinePunctuation({
					model: {
						ctTransformer: PUNCT_ONNX,
						numThreads: 2,
						provider: 'cpu',
						debug: 0
					}
				});
				LOG('标点模型已加载');
			} catch (e) {
				LOG('标点模型加载失败，将跳过加标点:', e.message);
			}
		} else {
			LOG(`未找到标点模型 ${PUNCT_ONNX}，将跳过加标点`);
		}
	}

	const makeVad = () => new sherpa.Vad({
		sileroVad: {
			model: VAD_ONNX,
			threshold: 0.5,
			minSilenceDuration: 0.4,
			minSpeechDuration: 0.2,
			maxSpeechDuration: 20,
			windowSize: FRAME
		},
		sampleRate: SAMPLE_RATE,
		numThreads: 1,
		provider: 'cpu',
		debug: 0
	}, MAX_RECORD_SECONDS + 5);

	/** 识别整段样本，返回文字 */
	function transcribe(samples) {
		const vad = makeVad();
		const texts = [];
		/* 首段补齐：VAD 判定语音的起点通常略晚于真实开口位置，
		 * 不补一小段前置音频会吃掉第一个音节（例如 world → role）。 */
		const onsetPadding = Math.round(ONSET_PADDING_SECONDS * SAMPLE_RATE);
		let isFirstSegment = true;
		const drain = () => {
			while (!vad.isEmpty()) {
				const seg = vad.front(false);
				let speech = seg.samples;
				if (isFirstSegment) {
					isFirstSegment = false;
					const from = Math.max(0, seg.start - onsetPadding);
					if (from < seg.start) {
						const restored = new Float32Array(seg.start - from + speech.length);
						restored.set(samples.subarray(from, seg.start));
						restored.set(speech, seg.start - from);
						speech = restored;
					}
				}
				const stream = recognizer.createStream();
				stream.acceptWaveform({ sampleRate: SAMPLE_RATE, samples: speech });
				recognizer.decode(stream);
				const t = recognizer.getResult(stream).text.trim();
				if (t) texts.push(t);
				vad.pop();
			}
		};
		for (let o = 0; o < samples.length; o += FRAME) {
			vad.acceptWaveform(samples.subarray(o, o + FRAME));
			drain();
		}
		vad.flush();
		drain();
		/* 各段先拼接，再统一加标点：上下文更完整，跨段衔接更自然。
		 * 中文不加分隔符；含英文时用空格分隔避免粘连。 */
		const hasLatin = texts.some((t) => /[A-Za-z]/.test(t));
		let out = texts.join(hasLatin ? ' ' : '').trim();
		if (punct && out) {
			try {
				out = punct.addPunct(out);
			} catch (e) {
				LOG('加标点失败，返回原始文本:', e.message);
			}
		}
		return out;
	}

	return { transcribe, makeVad, sherpa };
}


/* ---------------- 焦点诊断 ---------------- */
const XFOCUS = join(ROOT, 'bin', 'xfocus');
/** 把当前 X11 键盘焦点窗口的 ID 与标题写进日志 */
function logFocusTarget() {
	execFile(XFOCUS, [], { timeout: 3000 }, (err, stdout) => {
		if (err) return;
		const wid = String(stdout).trim();
		if (!wid || wid === '0x0') { LOG('[focus] 没有焦点窗口'); return; }
		execFile('xprop', ['-id', wid, 'WM_NAME', 'WM_CLASS'], { timeout: 3000 }, (e2, out) => {
			const name = e2 ? '' : String(out).replace(/\s+/g, ' ').trim().slice(0, 120);
			LOG(`[focus] 注入目标 ${wid} :: ${name}`);
		});
	});
}

/* ---------------- 注入 ---------------- */
/* paste（默认）：占有 CLIPBOARD 后模拟 Ctrl+V，不受键盘映射/输入法影响，最稳
 * type：逐字符 XTEST 重映射，适用于不支持粘贴的终端（需配 --ctrl-shift） */
const INJECT_MODE = process.env.VOICE_TYPER_INJECT ?? 'paste';
const INJECT_TERMINAL = process.env.VOICE_TYPER_TERMINAL === '1';

function inject(text) {
	return new Promise((resolve, reject) => {
		if (!text) return resolve();
		const args = INJECT_MODE === 'type'
			? ['--delay', '3', text]
			: (INJECT_TERMINAL ? ['--ctrl-shift', text] : [text]);
		const bin = INJECT_MODE === 'type' ? XTYPE : XPASTE;
		/* 记录注入瞬间的焦点窗口：粘贴失败时据此判断文字到底送去了哪里 */
		logFocusTarget();
		execFile(bin, args, { timeout: 60000 }, (err) => {
			if (err) reject(err); else resolve();
		});
	});
}

/* ---------------- 守护进程 ---------------- */
class Daemon {
	constructor() {
		this.state = 'loading';
		this.engine = null;
		this.proc = null;         // parecord 进程
		this.vad = null;
		this.carry = null;        // 未对齐到帧的残留样本
		this.chunks = [];
		this.sampleCount = 0;
		this.heardSpeech = false;
		this.silentMs = 0;
		this.startedAt = 0;
		this.finishing = false;
	}

	async init() {
		const t0 = Date.now();
		this.engine = createEngine();
		this.state = 'idle';
		LOG(`模型加载完成，用时 ${((Date.now() - t0) / 1000).toFixed(1)}s`);
	}

	broadcast(msg) {
		LOG(msg);
	}

	/** 开始录音 */
	async start() {
		if (this.state === 'recording') return { ok: false, error: '已经在录音' };
		if (this.state !== 'idle') return { ok: false, error: `当前状态 ${this.state}，不能开始` };

		this.state = 'recording';
		this.chunks = [];
		this.sampleCount = 0;
		this.heardSpeech = false;
		this.silentMs = 0;
		this.carry = new Float32Array(0);
		this.startedAt = Date.now();
		this.vad = this.engine.makeVad();

		this.proc = spawn('parecord', [
			'--format=s16le', '--rate=16000', '--channels=1',
			'--raw', '--latency-msec=50'
		], { stdio: ['ignore', 'pipe', 'pipe'] });

		this.proc.stderr.on('data', (d) => {
			const s = d.toString().trim();
			if (s) LOG('parecord:', s);
		});

		this.proc.on('error', (e) => {
			LOG('无法启动 parecord:', e.message);
			this.state = 'idle';
			notify('语音输入失败', 'parecord 启动失败: ' + e.message, 'critical');
		});

		let rest = Buffer.alloc(0);
		this.proc.stdout.on('data', (buf) => {
			if (this.state !== 'recording') return;
			const data = rest.length ? Buffer.concat([rest, buf]) : buf;
			const even = data.length - (data.length % 2);
			rest = data.subarray(even);
			const n = even / 2;
			if (n === 0) return;

			const samples = new Float32Array(n);
			for (let i = 0; i < n; i++) samples[i] = data.readInt16LE(i * 2) / 32768;
			this.chunks.push(samples);
			this.sampleCount += n;

			// 逐帧喂给 VAD
			const merged = this.carry.length
				? Float32Array.from([...this.carry, ...samples])
				: samples;
			let off = 0;
			while (off + FRAME <= merged.length) {
				const frame = merged.subarray(off, off + FRAME);
				this.vad.acceptWaveform(frame);
				off += FRAME;
			}
			this.carry = merged.subarray(off);

			// 自动停止判定（默认关闭，走「按两次」模式）
			if (AUTO_STOP) {
				const detected = this.vad.isDetected();
				if (detected) {
					this.heardSpeech = true;
					this.silentMs = 0;
				} else if (this.heardSpeech) {
					this.silentMs += (n / SAMPLE_RATE) * 1000;
					if (this.silentMs >= AUTO_STOP_SILENCE_MS) this.finish('auto');
				}
			}
			if ((Date.now() - this.startedAt) / 1000 >= MAX_RECORD_SECONDS) this.finish('timeout');
		});

		beep('start');
		indicatorShow();
		return { ok: true, state: this.state };
	}

	/** 停止录音并识别 */
	async finish(reason = 'manual') {
		if (this.state !== 'recording' || this.finishing) return { ok: false, error: '没有在录音' };
		this.finishing = true;
		this.state = 'transcribing';

		const proc = this.proc;
		this.proc = null;
		if (proc) {
			let exited = false;
			const done = new Promise((r) => { proc.once('close', () => { exited = true; r(); }); });
			proc.kill('SIGTERM');
			await Promise.race([done, new Promise((r) => setTimeout(r, KILL_GRACE_MS))]);
			if (!exited) { try { proc.kill('SIGKILL'); } catch {} }
		}
		beep('stop');
		indicatorHide();

		// 拼接全部样本
		const total = this.chunks.reduce((a, c) => a + c.length, 0);
		const all = new Float32Array(total);
		let p = 0;
		for (const c of this.chunks) { all.set(c, p); p += c.length; }
		this.chunks = [];

		const seconds = total / SAMPLE_RATE;
		this.finishing = false;

		if (seconds < MIN_RECORD_SECONDS) {
			this.state = 'idle';
			notify('没听到声音', '录制时间太短', 'low');
			return { ok: false, error: '录音过短', seconds };
		}

		let text = '';
		try {
			const t0 = Date.now();
			text = this.engine.transcribe(all);
			LOG(`识别完成 ${(seconds).toFixed(1)}s 音频 → ${((Date.now() - t0) / 1000).toFixed(2)}s: "${text}"`);
		} catch (e) {
			this.state = 'idle';
			notify('识别失败', e.message, 'critical');
			return { ok: false, error: e.message };
		}

		this.state = 'idle';
		if (!text) {
			notify('没有识别到文字', `${seconds.toFixed(1)}秒音频`, 'low');
			return { ok: true, text: '', injected: false, seconds };
		}

		try {
			await inject(text);
			/* 识别成功不弹任何提示，靠光标处的文字本身作反馈 */
			return { ok: true, text, injected: true, seconds, reason };
		} catch (e) {
			notify('输入失败', e.message, 'critical');
			return { ok: true, text, injected: false, error: e.message, seconds };
		}
	}

	status() {
		return {
			ok: true,
			state: this.state,
			seconds: this.state === 'recording' ? (Date.now() - this.startedAt) / 1000 : 0
		};
	}

	async handle(cmd) {
		const verb = cmd.split(/\s+/, 1)[0];
		switch (verb) {
			case 'toggle':
				if (this.state === 'recording') return await this.finish('manual');
				if (this.state === 'idle') return await this.start();
				return { ok: false, error: `忙碌中: ${this.state}` };
			case 'start': return await this.start();
			case 'stop': return await this.finish('manual');
			case 'inject': {
				/* 诊断用：直接把给定文字走一遍注入流程，不解录音 */
				const text = cmd.slice(7).trim();
				if (!text) return { ok: false, error: 'inject 需要文字' };
				try {
					await inject(text);
					LOG(`[inject] 已注入 ${text.length} 字（模式 ${INJECT_MODE}）`);
					return { ok: true, text, injected: true, mode: INJECT_MODE };
				} catch (e) {
					LOG('[inject] 失败:', e.message);
					return { ok: false, error: e.message };
				}
			}
			case 'status': return this.status();
			case 'quit': return { ok: true, quit: true };
			default: return { ok: false, error: '未知命令: ' + cmd };
		}
	}
}

/* ---------------- 客户端 ---------------- */
function send(cmd, timeoutMs = 120000) {
	return new Promise((resolve) => {
		const sock = net.createConnection(SOCK);
		let buf = '';
		let done = false;
		const finish = (v) => { if (!done) { done = true; sock.destroy(); resolve(v); } };
		sock.setTimeout(timeoutMs, () => finish({ ok: false, error: '守护进程响应超时' }));
		sock.on('connect', () => sock.write(cmd + '\n'));
		sock.on('data', (d) => {
			buf += d.toString();
			const nl = buf.indexOf('\n');
			if (nl >= 0) {
				try { finish(JSON.parse(buf.slice(0, nl))); }
				catch { finish({ ok: false, error: '响应解析失败: ' + buf }); }
			}
		});
		sock.on('error', (e) => finish({ ok: false, error: e.code === 'ENOENT' || e.code === 'ECONNREFUSED' ? 'NOT_RUNNING' : e.message }));
	});
}

async function ensureDaemon() {
	const probe = await send('status', 3000);
	if (probe.ok || probe.error !== 'NOT_RUNNING') return true;

	LOG('守护进程未运行，正在启动…');
	const child = spawn(process.execPath, [fileURLToPath(import.meta.url), 'dmn'], {
		detached: true, stdio: 'ignore', env: process.env
	});
	child.unref();

	for (let i = 0; i < 120; i++) {
		await new Promise((r) => setTimeout(r, 500));
		const s = await send('status', 3000);
		if (s.ok && s.state === 'idle') return true;
		if (s.ok && s.state === 'loading') continue;
	}
	return false;
}

/* ---------------- 入口 ---------------- */
const cmd = process.argv[2] ?? 'status';

if (cmd === 'dmn') {
	const d = new Daemon();
	/* 先清掉上一轮遗留的指示器通知，避免重启后堆在通知中心里 */
	indicatorCleanupStale();
	d.init().then(() => {
		if (existsSync(SOCK)) { try { unlinkSync(SOCK); } catch {} }
		const server = net.createServer((conn) => {
			let buf = '';
			conn.on('data', async (data) => {
				buf += data.toString();
				const nl = buf.indexOf('\n');
				if (nl < 0) return;
				const c = buf.slice(0, nl).trim();
				buf = buf.slice(nl + 1);
				const res = await d.handle(c);
				conn.write(JSON.stringify(res) + '\n');
				if (res.quit) { conn.end(); server.close(); process.exit(0); }
			});
		});
		server.listen(SOCK, () => {
			LOG(`守护进程就绪，套接字 ${SOCK}`);
			notify('语音输入已就绪', '按快捷键开始说话');
		});
		process.on('SIGTERM', () => { try { unlinkSync(SOCK); } catch {} process.exit(0); });
		process.on('SIGINT', () => { try { unlinkSync(SOCK); } catch {} process.exit(0); });
	}).catch((e) => {
		LOG('启动失败:', e.message);
		notify('语音输入启动失败', e.message, 'critical');
		process.exit(1);
	});
} else if (cmd === 'clean-notifications') {
	/* 清理通知中心里堆积的通知。
	 * freedesktop 通知规范没有「枚举」接口，只能按 ID 逐个 CloseNotification；
	 * 注意这会关掉所有应用的通知，不只是 voice-typer 的。 */
	const max = Number(process.argv[3] ?? 400);
	console.log(`正在清理 ID 1-${max} 的通知（会关掉所有应用的通知）…`);
	let closed = 0;
	const closeOne = (id) => new Promise((res) => {
		execFile('gdbus', ['call', '--session',
			'--dest', 'org.freedesktop.Notifications',
			'--object-path', '/org/freedesktop/Notifications',
			'--method', 'org.freedesktop.Notifications.CloseNotification', String(id)],
			(err, stdout) => {
				if (!err && /\(true,\)|\(\)/.test(String(stdout).trim())) closed++;
				res();
			});
	});
	for (let id = 1; id <= max; id++) {
		await closeOne(id);
		if (id % 50 === 0) process.stdout.write(`.`);
	}
	// 顺带清掉我们自己记录的指示器 ID
	writeIndicatorId(null);
	console.log(`\n完成，关闭了 ${closed} 条通知。`);
} else if (cmd === 'quit') {
	const r = await send('quit', 5000);
	console.log(JSON.stringify(r));
} else if (cmd === 'status') {
	const r = await send('status', 5000);
	console.log(JSON.stringify(r));
} else if (['toggle', 'start', 'stop'].includes(cmd) || cmd === 'inject') {
	const ok = await ensureDaemon();
	if (!ok) { console.error('守护进程启动失败'); process.exit(1); }
	const payload = cmd === 'inject'
		? 'inject ' + process.argv.slice(3).join(' ')
		: cmd;
	const r = await send(payload);
	console.log(JSON.stringify(r));
	if (cmd !== 'inject' && r.text) process.stdout.write(r.text + '\n');
} else {
	console.error(`用法: voice-typer [dmn|toggle|start|stop|status|quit|inject 文字]
  dmn      前台运行守护进程
  toggle   开始录音；再次调用则停止、识别并输入
  start    开始录音
  stop     停止、识别并输入
  status   查看状态
  quit     关闭守护进程
  inject   直接把给定文字注入光标处（诊断用）`);
	process.exit(1);
}
