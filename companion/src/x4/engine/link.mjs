// Links news stories to official acts with LátóTér's own claim clustering.
//   node link.mjs <engine folder>   < {"stories":[{"id","titles":[...]}],"acts":[{"id","title"}]}
//   > {"links":[["<story id>","<act id>"], ...]}
//
// The engine's scripts/cluster-claims.mjs groups items that state the same claim. Acts go in
// as constitutive items, the stories' headlines as press items; an act that lands in a
// story's cluster is that story's primary source. Kept in the companion: the engine is read-only.
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const engine = process.argv[2];
const { clusterClaims } = await import(pathToFileURL(join(engine, "scripts", "cluster-claims.mjs")).href);

let input = "";
for await (const chunk of process.stdin) input += chunk;
const { stories = [], acts = [] } = JSON.parse(input || "{}");

const items = [
  ...stories.flatMap((s) => (s.titles ?? []).map((title) => ({ title, brand: "story", story: s.id, link: s.id }))),
  ...acts.map((a) => ({ title: a.title, brand: "kozlony", constitutive: true, act: a.id, link: a.id })),
];
const byLink = new Map(items.map((it) => [it.link + "\u0000" + it.title, it]));
const links = new Set();
for (const c of clusterClaims(items)) {
  const members = c.members.map((m) => byLink.get(m.link + "\u0000" + m.title)).filter(Boolean);
  const storyIds = [...new Set(members.filter((m) => m.story !== undefined).map((m) => m.story))];
  const actIds = [...new Set(members.filter((m) => m.act !== undefined).map((m) => m.act))];
  for (const s of storyIds) for (const a of actIds) links.add(JSON.stringify([s, a]));
}
process.stdout.write(JSON.stringify({ links: [...links].map((l) => JSON.parse(l)) }) + "\n");
