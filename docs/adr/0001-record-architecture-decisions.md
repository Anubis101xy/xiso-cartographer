# 0001. Record architecture decisions

- **Status:** Accepted
- **Date:** 2026-10-13

## Context

xiso-cartographer makes several choices that are expensive to undo: dependencies (nxdk, BearSSL), on-disk and wire formats (map, catalog, journal), protocols and the licence. Contributors, including AI coding agents, need to know why each choice was made so they don't reopen settled questions or quietly break the assumptions behind them.

## Decision

We record every hard-to-reverse decision as an Architecture Decision Record (ADR), following Michael Nygard's format.

- ADRs live in `docs/adr/` and are named `NNNN-short-title.md`, numbered sequentially from `0001`.
- Each ADR has a **Status** (Proposed, Accepted, Deprecated, or Superseded by NNNN), a **Date**, and three sections: **Context**, **Decision** and **Consequences**.
- An ADR is added in the same MR as the change it justifies, or in an earlier docs-only MR.
- Accepted ADRs are not rewritten. To change a decision, write a new ADR and mark the old one "Superseded by NNNN".

## Consequences

- Reviewers can ask "where is the ADR?" for any new dependency, format, protocol or licence change.
- The reasoning behind a decision survives after the MR discussion is forgotten.
- There is a small writing cost per decision, which is acceptable for choices of this weight.

## ADR index

| # | Topic | Lands with |
|---|---|---|
| 0002 | Licence and dependency compatibility | MR-1 (accepted) |
| 0003 | Unit test framework (Unity) | MR-2 (accepted) |
| 0004 | Streaming install | MR-5 (accepted) |
| 0005 | BearSSL for TLS | MR-5 (accepted) |
| 0006 | No links, no bypasses, good-guest networking | MR-5 (accepted; amended by 0007) |
| 0007 | Scoped user credentials for sources that need a login | Docs PR before MR-8 (accepted; implemented in M3–M6; amended by 0010) |
| 0008 | Refuse games whose files FATX can't store; choose the install folder name | M2 (accepted) |
| 0009 | Install from zip archives by streaming the zip itself | Accepted, amended 2026-10-19 (adaptive spool); implemented in M5b |
| 0010 | Logging in on the Xbox is required for the beta | Accepted; built in M8, verified in M9 |
| 0011 | Resume inside large files from checkpoints (amends 0004 §4) | Accepted; M4 |
| 0012 | TLS trust: CA bundle, time source and randomness | Accepted; M5 |
| 0013 | The archive.org source: entries keyed by file name | Accepted; M6 |
| 0014 | The catalog: Wikidata and Redump data, no art, a checked binary file | Accepted; M7 |
| 0015 | The UI: SDL2 and SDL_ttf, with our own controller-first widgets | Accepted; M8 |
| 0016 | A settings file, and the install location in it | Accepted; M8 |
| 0017 | Releases: a tagged zip with every licence, built by CI as a draft | Proposed; M9 |
| 0018 | NAS shared folders over SMB: libsmb2, NT-hash logins, `@smb` folders, game folders, a scan, and PLAY | Accepted; owner of this fork |
