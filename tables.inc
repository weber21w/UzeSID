/* Runtime-generated envelope rates.
 *
 * Older development snapshots also carried four 256-entry combined-waveform
 * tables and a 256-byte envelope-shift table here. The lightweight UzeSID
 * renderer never referenced them, so keeping them only obscured what is live
 * and could waste flash on toolchains without effective section GC.
 */
static u16 eg_table[16];
