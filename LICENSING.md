# Licensing

GBCPP-Engine is **dual-licensed**.

## 1. Open-source license — AGPL-3.0

The engine is available under the **GNU Affero General Public License, version 3.0**
(see [`LICENSE`](LICENSE)). Under the AGPL you may use, modify, and distribute the
engine, including over a network, provided you make the complete corresponding
source of your version — engine and the application linked with it — available to
its users under the same terms.

Because the shipped product links the engine and the consuming application into a
single binary, the AGPL's obligations extend to the application linked against the
engine. Every fork of the engine inherits this license.

## 2. Commercial license

For consumers who cannot meet the AGPL's source-availability terms — for example,
shipping a closed-source product built on the engine — a separate **commercial
license** is available. The commercial license grants use of the engine without the
AGPL's copyleft obligations.

To obtain a commercial license, contact the maintainer.

## Choosing

| You are… | Use |
|---|---|
| Building open-source software you can release under AGPL-3.0 | The AGPL-3.0 license — no further action needed |
| Building a closed-source or otherwise AGPL-incompatible product | The commercial license — contact the maintainer |

## Vendored dependencies

Third-party code vendored under [`third_party/`](third_party/) retains its own
license. **SameBoy** (`third_party/sameboy/`) is MIT-licensed; its `LICENSE` file
stays intact in its subtree and is not relicensed. MIT is compatible with the
AGPL-3.0; the dual-license posture above applies to the engine's own code, not to
vendored dependencies.
