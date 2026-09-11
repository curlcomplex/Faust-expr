# Clap v0.1

Independent electronic clap voice developed under #51. Behavioral prior art: Elektron Syntakt CP Vintage documents a spacing/crunch control for the initial clap triggers, plus decay, body/noise balance, body character and overdrive. Community examples show the machine reaching well beyond a narrow handclap role. This implementation uses those ideas only as behavioral references; it does not recover or claim Elektron's internal circuit/DSP.

Architecture: four staggered shaped-noise events, a separately timed diffuse filtered-noise tail, optional short two-mode tonal body, and drive. Six visible synthesis controls: **Spacing / Punch / Decay / Color / Body / Drive**. Pitch, gate and velocity remain performance inputs. All values latch at onset; note-off does not choke.

External listening references (not bundled calibration assets):
- Elektron Syntakt manual, CP Vintage appendix: https://www.elektron.se/wp-content/uploads/2025/01/Syntakt-User-Manual_ENG_OS1.30B_250129.pdf
- Elektronauts CP Vintage Science Lab: https://www.elektronauts.com/t/syntakt-science-lab-5-cp-vintage/237671
- Example audio from that thread: https://eu5.dh-cdn.net/uploads/db8181/original/4X/8/0/6/80651acd8fd414dc09375957ae137d98cb0ad8e7.mp3

Go/no-go: compare directly with `snare-analog` and `snare-pm`. If the useful sound/control territory is not distinct enough, close #51 as not planned instead of keeping a redundant catalogue slot.
