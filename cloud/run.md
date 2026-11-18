# Without the Mac

The X4 can take its issue from a private GitHub repo on any saved Wi-Fi network, and send its
highlights and reading back up to the same repo. A scheduled Claude Code run in the cloud writes
the issue there each morning. The Mac is then only needed for books, their glossaries and
firmware.

## Set it up once

1. Make a private GitHub repo for your home, say `you/x4-data`, and make `~/x4` a checkout of it:

   ```bash
   cd ~/x4 && git init -b main && git add -A && git commit -m "my paper" && git remote add origin git@github.com:you/x4-data.git && git push -u origin main
   ```

   The starter `.gitignore` keeps tokens, calendar addresses and the football key out of it.

2. Give the X4 a fine-grained GitHub token for that repo alone, with Contents set to read and
   write. With the X4 plugged in:

   ```bash
   x4 repo set you/x4-data
   ```

3. In Claude Code on the web, make an environment that can clone this repo and `x4-data`. Paste
   [setup.sh](setup.sh) into its setup script. If you follow football, add your football-data.org
   key as a secret that the environment adds to requests to `api.football-data.org`.

4. Make a routine on that environment, early each morning, with the prompt below. Run it once by
   hand first and read its report.

## The prompt

You are the X4's writer, run in the cloud. No X4 is attached and nobody is watching, so never
stop to ask: where the skill leaves a choice, take the option it recommends and write down each
assumption for the report.

1. Find the two clones, `x4r` and `x4-data`, each a git checkout under `$HOME` or the working
   directory.
2. In every shell you open:

   ```bash
   export X4_HOME=<x4-data> TZ=<your time zone, e.g. Europe/Berlin>
   mkdir -p ~/bin && printf '#!/bin/sh\nexec uv run --project <x4r>/companion x4 "$@"\n' > ~/bin/x4 && chmod +x ~/bin/x4
   export PATH=~/bin:$PATH
   ```

   When the environment adds the football key for you, also `export X4_FOOTBALL_KEY=proxy`.
3. In `x4-data`, work on `main`: `x4 stage` commits there and pushes, and the X4 takes its issue
   from `main`. If git has no `user.name` there, set `x4 writer` and `x4-writer@localhost` in that
   clone only.
4. Follow `<x4r>/.claude/skills/x4-sync/SKILL.md`, the unplugged run, from step 1 to the end,
   reading `$X4_HOME` wherever it says `~/x4`.
5. Change nothing in `x4r` and push nothing yourself. The only push is the one `x4 stage` makes.
6. When `x4 stage` refuses a file, fix what it names and run it again. When it fails for any
   other reason, stop there and say so.
7. Report in five lines at most: the skill's own report, then every assumption you made and
   every warning `x4 feed` or `x4 stage` gave.
