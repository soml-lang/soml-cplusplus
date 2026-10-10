/*
Differential testing against the JavaScript reference implementation in `../soml-javascript`, which must be checked out next to this repository. Run it with `node tests/differential.js [count] [seed]` and `SOML_TAGGED` set to the built `soml-tagged`, with Node.js 26 or later, which the reference implementation needs for Temporal.

It generates documents in random spellings, and mutates them byte by byte and token by token. For every input, both implementations must accept it or both must reject it. When both accept it, both must write exactly the same canonical form, and when both reject it, both must report the error at the same line and column. The messages are compared too.

Arguments: the number of inputs (default 20000) and a seed (default 1).
*/
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const referencePath = path.join(import.meta.dirname, '..', '..', 'soml-javascript', 'source', 'index.ts');
const {parse, stringify} = await import(referencePath);
const tool = process.env.SOML_TAGGED ?? path.join(import.meta.dirname, '..', 'build', 'soml-tagged');
const count = Number(process.argv[2] ?? 20_000);
let state = BigInt(process.argv[3] ?? 1) * 0x9E37_79B9_7F4A_7C15n || 1n;

function random() {
	state ^= (state << 13n) & 0xFFFF_FFFF_FFFF_FFFFn;
	state ^= state >> 7n;
	state ^= (state << 17n) & 0xFFFF_FFFF_FFFF_FFFFn;
	return Number(state & 0xFF_FFFFn) / 0x100_0000;
}

function integer(minimum, maximum) {
	return minimum + Math.floor(random() * (maximum - minimum + 1));
}

function pick(items) {
	return items[integer(0, items.length - 1)];
}

const keyCharacters = ['a', 'b', 'z', 'A', '0', '9', '_', '-', '.', ' ', '\'', '"', 'é', '😀', '\\', '\t', ':'];

function randomKey() {
	const length = integer(0, 4);
	let key = '';

	for (let index = 0; index < length; index++) {
		key += pick(keyCharacters);
	}

	return key;
}

function writeKey(key) {
	if (key !== '' && /^[\w\-]+$/v.test(key) && random() < 0.7) {
		return key;
	}

	if (!key.includes('\'') && !key.includes('\n') && random() < 0.5) {
		return `'${key}'`;
	}

	return `"${escapeString(key)}"`;
}

function escapeString(string) {
	let output = '';

	for (const character of string) {
		const code = character.codePointAt(0);

		if (character === '\\' || character === '"') {
			output += `\\${character}`;
		} else if (character === '\n') {
			output += random() < 0.5 ? '\\n' : '\\u{a}';
		} else if (character === '\t') {
			output += random() < 0.5 ? '\\t' : '\t';
		} else if (code < 0x20 || code === 0x7F || random() < 0.05) {
			output += `\\u{${code.toString(16)}}`;
		} else {
			output += character;
		}
	}

	return output;
}

const stringCharacters = ['a', ' ', '\'', '"', '\\', '\n', '\t', 'é', '😀', '\u{0}', '\u{1}', '\u{10}', '\u{1F}', '\u{7F}', '\u{2028}', '#', '/*'];

function randomString() {
	const length = integer(0, 6);
	let string = '';

	for (let index = 0; index < length; index++) {
		string += pick(stringCharacters);
	}

	return string;
}

function writeString(string) {
	const choice = random();

	if (choice < 0.3 && !/['\n\u{0}-\u{8}\u{B}-\u{1F}\u{7F}]/v.test(string)) {
		return `'${string}'`;
	}

	if (choice < 0.45 && string !== '' && !/^[\t ]*$|\n[\t ]*\n|\n[\t ]*$|^[\t ]*\n|'''|[\u{0}-\u{8}\u{B}-\u{1F}\u{7F}]/v.test(string) && !string.split('\n').some(line => /^[\t ]*$/v.test(line))) {
		const indentation = pick(['', '\t', '  ']);
		return `'''\n${indentation}${string.replaceAll('\n', `\n${indentation}`)}\n${indentation}'''`;
	}

	return `"${escapeString(string)}"`;
}

function withUnderscores(digits) {
	let output = '';

	for (const [index, digit] of [...digits].entries()) {
		if (index > 0 && random() < 0.15) {
			output += '_';
		}

		output += digit;
	}

	return output;
}

function randomScalar() {
	switch (integer(0, 9)) {
		case 0: {
			return writeString(randomString());
		}

		case 1: {
			const value = BigInt(integer(-1_000_000, 1_000_000)) * BigInt(pick([1, 1, 1000, 1_000_000_007, 9_223_372_036]));
			const spelling = random();

			if (value >= 0n && spelling < 0.15) {
				return `0x${withUnderscores(value.toString(16).toUpperCase())}`;
			}

			if (value >= 0n && spelling < 0.25) {
				return `0o${withUnderscores(value.toString(8))}`;
			}

			if (value >= 0n && spelling < 0.3) {
				return `0b${withUnderscores(value.toString(2))}`;
			}

			return (value < 0n ? '-' : '') + withUnderscores((value < 0n ? -value : value).toString());
		}

		case 2: {
			return pick(['9223372036854775807', '-9223372036854775808', '9223372036854775808', '0', '-0', '00', '0x', '0xff', '-0x1', '1__0', '_1']);
		}

		case 3: {
			const mantissa = `${integer(0, 99_999)}.${integer(0, 999_999)}`;
			const exponent = random() < 0.5 ? `e${pick(['', '-'])}${integer(0, 330)}` : '';
			return pick(['', '-']) + mantissa + exponent;
		}

		case 4: {
			return pick(['infinity', '-infinity', '1e308', '1e309', '5e-324', '2.4703282292062328e-324', '2.4703282292062327e-324', '1e-400', '0.0e999', '1.0', '1e0', '1e-0', '1E5', '1.', '.5', '0.1e-5', '123456789012345678901234567890.5']);
		}

		case 5: {
			const pad = number => String(number).padStart(2, '0');
			const date = `${String(integer(1, 9999)).padStart(4, '0')}-${pad(integer(1, 12))}-${pad(integer(1, 31))}`;
			const time = `T${pad(integer(0, 24))}:${pad(integer(0, 59))}:${pad(integer(0, 60))}`;
			const fraction = random() < 0.3 ? `.${String(integer(0, 999_999_999)).slice(0, integer(1, 10))}` : '';
			const offset = random() < 0.5 ? 'Z' : `${pick(['+', '-'])}${pad(integer(0, 23))}:${pad(integer(0, 59))}`;
			return date + time + fraction + offset;
		}

		case 6: {
			const units = ['h', 'm', 's', 'ms', 'us', 'ns'];
			let text = random() < 0.2 ? '-' : '';
			let unitIndex = integer(0, 5);

			do {
				text += String(integer(0, 99_999));

				if (random() < 0.2) {
					text += `.${integer(0, 999)}`;
				}

				text += units[unitIndex];
				unitIndex += integer(1, 3);
			} while (unitIndex < units.length && random() < 0.5);

			return text;
		}

		case 7: {
			return pick(['true', 'false', 'null', 'nan', 'True', 'yes']);
		}

		case 8: {
			return pick(['2562047h47m16.854775807s', '-2562047h47m16.854775808s', '2562047h47m16.854775808s', '0.5ns', '1.0000000001s', '0.0000000001h', '-0s', '1d', '1h1h']);
		}

		default: {
			return String(integer(0, 100));
		}
	}
}

function trivia() {
	return pick(['', '', '', ' ', '\t', ' # comment\n', ' /* c */ ', '\n', '\n\n']);
}

// Inside brackets, a comma, a line break, or both separate items.
function separator() {
	return pick([',', ',', '\n', ',\n', '\n,\n']);
}

function randomValue(depth) {
	const choice = random();

	if (depth > 3 || choice < 0.6) {
		return randomScalar();
	}

	if (choice < 0.8) {
		const items = Array.from({length: integer(0, 4)}, () => trivia() + randomValue(depth + 1) + trivia());
		return `[${items.map((item, index) => (index > 0 ? separator() : '') + item).join('')}${items.length > 0 && random() < 0.5 ? ',' : ''}]`;
	}

	const members = Array.from({length: integer(0, 4)}, () => `${trivia()}${randomEntryKey()}:${pick([' ', '', '\n'])}${randomValue(depth + 1)}${trivia()}`);
	return `{${members.map((member, index) => (index > 0 ? separator() : '') + member).join('')}${members.length > 0 && random() < 0.5 ? ',' : ''}}`;
}

function randomEntryKey() {
	return writeKey(random() < 0.3 ? pick(['a', 'b', 'c']) : randomKey());
}

/*
Nesting around the limit of 100 levels, written with brackets, braces, or a brace-less top level.
*/
function deepDocument() {
	const depth = integer(97, 103);

	switch (integer(0, 2)) {
		case 0: {
			return '['.repeat(depth) + ']'.repeat(depth);
		}

		case 1: {
			return `${'{a: '.repeat(depth - 1)}{}${'}'.repeat(depth - 1)}`;
		}

		default: {
			return `a: ${'['.repeat(depth - 1)}${']'.repeat(depth - 1)}`;
		}
	}
}

function randomDocument() {
	const choice = random();

	if (choice < 0.02) {
		return deepDocument();
	}

	if (choice < 0.15) {
		return randomValue(0);
	}

	const entries = Array.from({length: integer(1, 6)}, () => `${randomEntryKey()}: ${randomValue(1)}`);
	return pick(['', '# start\n', '\n']) + entries.join(pick(['\n', '\n\n', ' # c\n', '\n/* x */\n'])) + pick(['', '\n', ' # end']);
}

const tokens = ['{', '}', '[', ']', ':', ',', '.', '\'', '"', '\n', ' ', '\t', '#', '/*', '*/', '\\', 'a', '1', '-', '_', 'e', 'x', 'T', 'Z', '\'\'\'\n', '\u{FEFF}', '\r', '\u{0}', 'é'];

function mutate(text) {
	const characters = [...text];
	const count = integer(1, 3);

	for (let index = 0; index < count; index++) {
		const position = integer(0, characters.length);
		const operation = random();

		if (operation < 0.4 && characters.length > 0) {
			characters.splice(Math.min(position, characters.length - 1), 1);
		} else if (operation < 0.8) {
			characters.splice(position, 0, pick(tokens));
		} else if (characters.length > 0) {
			characters[Math.min(position, characters.length - 1)] = pick(tokens);
		}
	}

	return characters.join('');
}

function referenceResult(bytes) {
	try {
		return {ok: true, canonical: stringify(parse(bytes), {canonical: true})};
	} catch (error) {
		if (error.name === 'ParseError') {
			return {ok: false, message: error.reason, line: error.line, column: error.column};
		}

		throw error;
	}
}

using directory = fs.mkdtempDisposableSync(path.join(os.tmpdir(), 'soml-differential-'));
const files = [];
const inputs = [];

for (let index = 0; index < count; index++) {
	let text = randomDocument();

	if (random() < 0.3) {
		text = mutate(text);
	}

	const file = path.join(directory.path, `${index}.soml`);
	const bytes = Buffer.from(text, 'utf8');

	// Some inputs are invalid UTF-8 on purpose.
	if (random() < 0.02 && bytes.length > 0) {
		bytes[integer(0, bytes.length - 1)] = pick([0x80, 0xC0, 0xED, 0xFF]);
	}

	fs.writeFileSync(file, bytes);
	files.push(file);
	inputs.push(bytes);
}

const output = execFileSync(tool, ['-'], {input: files.join('\n'), maxBuffer: 1024 * 1024 * 1024, encoding: 'utf8'});

const results = new Map(output.trim().split('\n').map(line => {
	const result = JSON.parse(line);
	return [result.file, result];
}));

let accepted = 0;
const disagreements = [];

for (const [index, file] of files.entries()) {
	const c = results.get(file);
	const reference = referenceResult(inputs[index]);

	const isSameError = c.ok || (c.line === reference.line && c.column === reference.column && c.message === reference.message);

	if (c.ok !== reference.ok || (c.ok && c.canonical !== reference.canonical) || !isSameError) {
		disagreements.push({file, text: inputs[index].toString('utf8'), cplusplus: c.ok ? c.canonical ?? c.writeError : `rejected at ${c.line}:${c.column}: ${c.message}`, reference: reference.ok ? reference.canonical : `rejected at ${reference.line}:${reference.column}: ${reference.message}`});
	}

	accepted += c.ok ? 1 : 0;
}

console.log(`${count} inputs, ${accepted} accepted by both or by C++, ${disagreements.length} disagreements`);

for (const disagreement of disagreements.slice(0, 10)) {
	console.log(JSON.stringify(disagreement, undefined, '\t'));
}

process.exitCode = disagreements.length > 0 ? 1 : 0;
