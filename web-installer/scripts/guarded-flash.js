// Adapted from esp-web-tools 10.4.0 (Apache-2.0). Board and SHA-256 guards added by WLAN Commander.
import SparkMD5 from "spark-md5";
import {validateBoard, verifyPart} from "./firmware-guards.mjs";
import { Transport, ESPLoader } from "esptool-js";
import { sleep } from "esp-web-tools/dist/util/sleep.js";
/**
 * Perform a hard reset using esploader's chip-specific reset logic.
 * First asserts RTS to enter reset, then calls esploader.after() which
 * uses the chip-specific reset procedure to release.
 */
const hardResetDevice = async (transport, esploader) => {
    await transport.setRTS(true);
    await sleep(100);
    await esploader.after();
};
export const flash = async (onEvent, port, manifestPath, manifest, eraseFirst) => {
    let build;
    let chipFamily;
    const fireStateEvent = (stateUpdate) => onEvent({
        ...stateUpdate,
        manifest,
        build,
        chipFamily,
    });
    const transport = new Transport(port);
    const portInfo = port.getInfo();
    const isCdcUsbPort = portInfo &&
        portInfo.usbVendorId === 0x303a &&
        portInfo.usbProductId !== undefined &&
        [0x1001, 0x1002, 0x1003, 0x0002, 0x0003].includes(portInfo.usbProductId);
    const esploader = new ESPLoader({
        transport,
        baudrate: 115200,
        enableTracing: false,
    });
    // For debugging
    window.esploader = esploader;
    fireStateEvent({
        state: "initializing" /* FlashStateType.INITIALIZING */,
        message: "Initializing...",
        details: { done: false },
    });
    try {
        await esploader.main();
        await esploader.flashId();
    }
    catch (err) {
        console.error(err);
        fireStateEvent({
            state: "error" /* FlashStateType.ERROR */,
            message: "Failed to initialize. Try resetting your device or holding the BOOT button while clicking INSTALL.",
            details: { error: "failed_initialize" /* FlashError.FAILED_INITIALIZING */, details: err },
        });
        await hardResetDevice(transport, esploader);
        await transport.disconnect();
        return;
    }
    chipFamily = esploader.chip.CHIP_NAME;
    fireStateEvent({
        state: "initializing" /* FlashStateType.INITIALIZING */,
        message: `Initialized. Found ${chipFamily}`,
        details: { done: true },
    });
    const detectedSerialType = isCdcUsbPort ? "cdc" : "uart";
    build =
        manifest.builds.find((b) => b.chipFamily === chipFamily && b.serialType === detectedSerialType) ||
            manifest.builds.find((b) => b.chipFamily === chipFamily && b.serialType === undefined);
    if (!build) {
        fireStateEvent({
            state: "error" /* FlashStateType.ERROR */,
            message: `Your ${chipFamily} board is not supported.`,
            details: { error: "not_supported" /* FlashError.NOT_SUPPORTED */, details: chipFamily },
        });
        await hardResetDevice(transport, esploader);
        await transport.disconnect();
        return;
    }
    try {
        const id = await esploader.readFlashId();
        validateBoard(manifest, chipFamily, id);
    } catch (err) {
        fireStateEvent({state: "error", message: err.message, details: {error: "not_supported", details: err.message}});
        await hardResetDevice(transport, esploader);
        await transport.disconnect();
        return;
    }
    fireStateEvent({
        state: "preparing" /* FlashStateType.PREPARING */,
        message: "Preparing installation...",
        details: { done: false },
    });
    const manifestURL = manifestPath.startsWith("blob:") || manifestPath.startsWith("data:")
        ? location.toString()
        : new URL(manifestPath, location.toString()).toString();
    const filePromises = build.parts.map(async (part) => {
        const url = new URL(part.path, manifestURL).toString();
        const resp = await fetch(url);
        if (!resp.ok) {
            throw new Error(`Downloading firmware ${part.path} failed: ${resp.status}`);
        }
        const reader = new FileReader();
        const blob = await resp.blob();
        return new Promise((resolve) => {
            reader.addEventListener("load", () => resolve(reader.result));
            reader.readAsArrayBuffer(blob);
        });
    });
    const fileArray = [];
    let totalSize = 0;
    for (let part = 0; part < filePromises.length; part++) {
        try {
            const buffer = await filePromises[part];
            const data = new Uint8Array(buffer, 0, buffer.byteLength);
            await verifyPart(build.parts[part], data);
            fileArray.push({ data, address: build.parts[part].offset });
            totalSize += data.length;
        }
        catch (err) {
            fireStateEvent({
                state: "error" /* FlashStateType.ERROR */,
                message: err.message,
                details: {
                    error: "failed_firmware_download" /* FlashError.FAILED_FIRMWARE_DOWNLOAD */,
                    details: err.message,
                },
            });
            await hardResetDevice(transport, esploader);
            await transport.disconnect();
            return;
        }
    }
    fireStateEvent({
        state: "preparing" /* FlashStateType.PREPARING */,
        message: "Installation prepared",
        details: { done: true },
    });
    if (eraseFirst) {
        fireStateEvent({
            state: "erasing" /* FlashStateType.ERASING */,
            message: "Erasing device...",
            details: { done: false },
        });
        await esploader.eraseFlash();
        fireStateEvent({
            state: "erasing" /* FlashStateType.ERASING */,
            message: "Device erased",
            details: { done: true },
        });
    }
    fireStateEvent({
        state: "writing" /* FlashStateType.WRITING */,
        message: `Writing progress: 0%`,
        details: {
            bytesTotal: totalSize,
            bytesWritten: 0,
            percentage: 0,
        },
    });
    let totalWritten = 0;
    try {
        await esploader.writeFlash({
            fileArray,
            flashSize: "keep",
            flashMode: "keep",
            flashFreq: "keep",
            eraseAll: false,
            compress: true,
            calculateMD5Hash: data => SparkMD5.ArrayBuffer.hash(data.buffer.slice(data.byteOffset,data.byteOffset+data.byteLength)),
            // report progress
            reportProgress: (fileIndex, written, total) => {
                const uncompressedWritten = (written / total) * fileArray[fileIndex].data.length;
                const newPct = Math.floor(((totalWritten + uncompressedWritten) / totalSize) * 100);
                // we're done with this file
                if (written === total) {
                    totalWritten += uncompressedWritten;
                    return;
                }
                fireStateEvent({
                    state: "writing" /* FlashStateType.WRITING */,
                    message: `Writing progress: ${newPct}%`,
                    details: {
                        bytesTotal: totalSize,
                        bytesWritten: totalWritten + written,
                        percentage: newPct,
                    },
                });
            },
        });
    }
    catch (err) {
        fireStateEvent({
            state: "error" /* FlashStateType.ERROR */,
            message: err.message,
            details: { error: "write_failed" /* FlashError.WRITE_FAILED */, details: err },
        });
        await hardResetDevice(transport, esploader);
        await transport.disconnect();
        return;
    }
    fireStateEvent({
        state: "writing" /* FlashStateType.WRITING */,
        message: "Writing complete",
        details: {
            bytesTotal: totalSize,
            bytesWritten: totalWritten,
            percentage: 100,
        },
    });
    await hardResetDevice(transport, esploader);
    console.log("DISCONNECT");
    await transport.disconnect();
    fireStateEvent({
        state: "finished" /* FlashStateType.FINISHED */,
        message: "All done!",
    });
};
