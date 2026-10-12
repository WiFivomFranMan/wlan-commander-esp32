export function validateBoard(manifest, family, flashId) {
  if (family !== 'ESP32-C5') throw new Error('This firmware requires ESP32-C5.');
  const capacityId = (flashId >>> 16) & 255;
  const flashBytes = capacityId >= 18 && capacityId <= 26 ? 2 ** capacityId : 0;
  if (!flashBytes || !Array.isArray(manifest.flash_sizes) || !manifest.flash_sizes.includes(flashBytes)) {
    throw new Error(`Wrong board for ${manifest.name}. Choose the matching screen and flash size before installing.`);
  }
  return flashBytes;
}
export async function verifyPart(part, data) {
  if (!Number.isSafeInteger(part.offset) || part.offset < 0 || part.bytes !== data.byteLength || !/^[a-f0-9]{64}$/.test(part.sha256 || '')) throw new Error('Firmware size or manifest is invalid. Nothing was written.');
  const digest = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', data)), b => b.toString(16).padStart(2,'0')).join('');
  if (digest !== part.sha256) throw new Error('Firmware checksum does not match. Nothing was written.');
}
