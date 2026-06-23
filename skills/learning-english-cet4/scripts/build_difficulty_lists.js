#!/usr/bin/env node

const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const source = path.join(root, "word.txt");

const outputs = {
  easy: path.join(root, "word.easy.txt"),
  medium: path.join(root, "word.medium.txt"),
  hard: path.join(root, "word.hard.txt"),
  veryHard: path.join(root, "word.very-hard.txt"),
};

const common = new Set([
  "a", "an", "the", "be", "is", "are", "was", "were", "do", "does", "did",
  "go", "get", "make", "take", "give", "come", "see", "know", "think",
  "use", "work", "look", "need", "want", "help", "call", "try", "ask",
  "feel", "keep", "let", "put", "mean", "leave", "move", "play", "run",
  "show", "turn", "start", "talk", "tell", "walk", "watch", "write",
  "good", "bad", "new", "old", "big", "small", "high", "low", "long",
  "short", "early", "late", "young", "right", "wrong", "easy", "hard",
  "happy", "sad", "hot", "cold", "near", "far", "fast", "slow", "free",
  "safe", "same", "open", "close", "clear", "clean", "full", "main",
  "home", "school", "student", "teacher", "friend", "family", "people",
  "person", "city", "country", "world", "day", "week", "month", "year",
  "time", "money", "food", "water", "room", "house", "car", "road",
  "book", "paper", "word", "line", "idea", "story", "job", "team",
  "life", "hand", "head", "face", "eye", "heart", "body", "music",
  "movie", "game", "shop", "price", "plan", "test", "class", "door",
]);

const academicSuffixes = [
  "tion", "sion", "ment", "ance", "ence", "ity", "ility", "ness",
  "ism", "ist", "ive", "ative", "ous", "eous", "ious", "ate", "ize",
  "ise", "fy", "ship", "hood", "graphy", "logy", "cracy",
];

const hardSignals = [
  "inter", "intra", "trans", "counter", "contra", "psycho", "physio",
  "bio", "geo", "hydro", "thermo", "electro", "micro", "macro", "multi",
  "poly", "proto", "pseudo", "hetero", "homo",
];

const easyPos = /^(art\.|pron\.|prep\.|conj\.|num\.|int\.)/i;
const corePos = /^(n\.|v\.|vt\.|vi\.|adj\.|adv\.)/i;

function parseLine(raw, index) {
  const trimmed = raw.trim();
  if (!trimmed) return null;
  const tab = trimmed.indexOf("\t");
  if (tab < 1) return null;
  const word = trimmed.slice(0, tab).trim();
  const meaning = trimmed.slice(tab + 1).trim().replace(/\s+/g, " ");
  if (!word || !meaning) return null;
  return { word, meaning, sourceLine: index + 1 };
}

function score(entry) {
  const word = entry.word.toLowerCase();
  const letters = word.replace(/[^a-z]/g, "");
  let s = 0;

  if (letters.length <= 3) s -= 2;
  else if (letters.length <= 5) s -= 1;
  else if (letters.length <= 7) s += 0;
  else if (letters.length <= 9) s += 1;
  else if (letters.length <= 11) s += 2;
  else s += 3;

  if (common.has(word)) s -= 2;
  if (word.includes("-") || word.includes(" ")) s += 1;

  const suffixHits = academicSuffixes.filter((suffix) => word.endsWith(suffix)).length;
  s += Math.min(2, suffixHits);

  if (hardSignals.some((prefix) => word.startsWith(prefix))) s += 1;
  if (/^(un|in|im|ir|il|dis|mis|non|over|under|anti|pre|post|re)[a-z]{5,}/.test(word)) s += 1;

  if (entry.meaning.length > 30) s += 1;
  if (entry.meaning.length > 55) s += 1;
  if (/[；;]/.test(entry.meaning)) s += 1;
  if (/抽象|哲学|学术|正式|罕见|专业|法律|经济|政治|心理|生物|技术/.test(entry.meaning)) s += 1;

  if (easyPos.test(entry.meaning)) s -= 1;
  else if (!corePos.test(entry.meaning)) s += 1;

  return s;
}

function bucket(entry) {
  const s = score(entry);
  if (s <= -1) return "easy";
  if (s <= 1) return "medium";
  if (s <= 4) return "hard";
  return "veryHard";
}

const rows = fs.readFileSync(source, "utf8").split(/\r?\n/).map(parseLine).filter(Boolean);
const unique = new Map();
for (const row of rows) {
  const key = row.word.toLowerCase();
  const prev = unique.get(key);
  if (!prev || row.meaning.length > prev.meaning.length) {
    unique.set(key, row);
  }
}

const buckets = { easy: [], medium: [], hard: [], veryHard: [] };
for (const entry of unique.values()) {
  buckets[bucket(entry)].push(entry);
}

for (const [name, file] of Object.entries(outputs)) {
  const body = buckets[name]
    .map((entry) => `${entry.word}\t${entry.meaning}`)
    .join("\n");
  fs.writeFileSync(file, `${body}\n`, "utf8");
}

for (const name of ["easy", "medium", "hard", "veryHard"]) {
  console.log(`${name}: ${buckets[name].length}`);
}
