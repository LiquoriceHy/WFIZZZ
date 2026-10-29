#!/usr/bin/env python3
"""Re-quantize YAMNet (mel-patch input) with REAL audio calibration.

The published chayuto/yamnet-mel-int8-tflm model outputs ~0.5 for every
input (verified 2026-10-01) - its synthetic random calibration is useless.
This script rebuilds the same graph from Google's yamnet.py + yamnet.h5 and
runs INT8 PTQ with representative log-mel patches computed from our own
23 snore/negative samples using the numpy frontend already validated
bit-exact against the float tflite graph.

Pipeline:
  1. mp3s -> 16k mono wavs (ffmpeg) in calib_wav/
  2. slide 15600-sample windows (hop 7800) -> mel patches (numpy frontend)
  3. build Keras mel-patch model, load yamnet.h5, VERIFY against float tflite
     scores on one patch (catches silent skip_mismatch weight-loading failure)
  4. INT8 PTQ -> yamnet_cal.tflite
  5. quick sanity: int8 vs float snore scores on 2 files

Run in the TF venv:  ~/tf_venv/bin/python quantize_real.py
"""
import glob
import os
import subprocess
import sys

import numpy as np

MCU = os.path.dirname(os.path.abspath(__file__))
AUDIO_DIR = os.environ.get('SNORE_AUDIO_DIR', '/home/user/snore_check/audio')
CALIB_WAV = os.path.join(MCU, 'calib_wav')
FLOAT_MODEL = os.environ.get(
    'SNORE_FLOAT_MODEL',
    '/home/user/snore_check/model/lite-model_yamnet_tflite_1.tflite')
OUT_MODEL = os.path.join(MCU, 'yamnet_cal.tflite')
SR = 16000
WIN = 15600
HOP = 7800
SNORE_IDX = 38
MAX_PER_FILE = 30        # cap windows per file so pos/neg stay balanced
TARGET_PATCHES = 400


def _import_tf():
    import tensorflow as tf
    sys.path.insert(0, MCU)
    import params as params_mod
    import yamnet as yamnet_mod
    return tf, params_mod, yamnet_mod


def decode16k(path):
    """16k mono float32. .wav via stdlib wave (docker-friendly), else ffmpeg."""
    if path.endswith('.wav'):
        import wave
        with wave.open(path, 'rb') as w:
            assert w.getframerate() == SR and w.getnchannels() == 1, path
            width = w.getsampwidth()
            raw = w.readframes(w.getnframes())
        assert width == 2, f'expect 16-bit pcm wav: {path}'
        return np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    out = subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', path, '-ac', '1', '-ar', str(SR),
         '-f', 'f32le', '-'], capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype=np.float32)


def make_wavs():
    os.makedirs(CALIB_WAV, exist_ok=True)
    wavs = []
    for mp3 in sorted(glob.glob(os.path.join(AUDIO_DIR, '*.mp3'))):
        base = os.path.splitext(os.path.basename(mp3))[0]
        wav = os.path.join(CALIB_WAV, base + '.wav')
        if not os.path.exists(wav):
            subprocess.run(['ffmpeg', '-v', 'error', '-i', mp3,
                            '-ac', '1', '-ar', str(SR), wav], check=True)
        wavs.append(wav)
    return wavs


def build_patches(wavs):
    hann = np.load(os.path.join(MCU, 'const_400.npy')).reshape(-1)
    mel = np.load(os.path.join(MCU, 'const_16448.npy')).astype(np.float32)

    def logmel(x):
        f = 1 + (len(x) - 400) // 160
        framed = np.lib.stride_tricks.as_strided(
            x, (f, 400), (x.strides[0] * 160, x.strides[0]))
        spec = np.abs(np.fft.rfft(framed * hann, n=512, axis=1))
        return np.log(spec @ mel.T + 0.001).astype(np.float32)

    patches, labels = [], []
    for wav in wavs:
        x = decode16k(wav)
        n = 1 + (len(x) - WIN) // HOP if len(x) >= WIN else 0
        if n == 0:
            continue
        step = max(1, n // MAX_PER_FILE)
        idxs = list(range(0, n, step))[:MAX_PER_FILE]
        for i in idxs:
            patches.append(logmel(x[i * HOP:i * HOP + WIN]))
            labels.append(1 if os.path.basename(wav).startswith('pos_') else 0)
    patches = np.stack(patches)
    labels = np.array(labels)
    if len(patches) > TARGET_PATCHES:
        rng = np.random.default_rng(0)
        keep = rng.permutation(len(patches))[:TARGET_PATCHES]
        patches, labels = patches[keep], labels[keep]
    print(f'calibration patches: {len(patches)} '
          f'(pos {int(labels.sum())} / neg {int((labels == 0).sum())})')
    return patches, labels


def verify_weights(keras_model):
    """Strong check: same audio window through (numpy frontend + Keras float)
    must match the float tflite reference scores. Random-init weights or a
    silent skip_mismatch failure would show a big delta."""
    from ai_edge_litert.interpreter import Interpreter
    hann = np.load(os.path.join(MCU, 'const_400.npy')).reshape(-1)
    mel = np.load(os.path.join(MCU, 'const_16448.npy')).astype(np.float32)
    x = decode16k(os.path.join(CALIB_WAV, 'pos_180338.wav'))[:WIN]
    f = 1 + (WIN - 400) // 160
    framed = np.lib.stride_tricks.as_strided(
        x, (f, 400), (x.strides[0] * 160, x.strides[0]))
    spec = np.abs(np.fft.rfft(framed * hann, n=512, axis=1))
    patch = np.log(spec @ mel.T + 0.001).astype(np.float32)

    pred = keras_model.predict(patch[np.newaxis], verbose=0)[0]
    itp = Interpreter(model_path=FLOAT_MODEL)
    itp.resize_tensor_input(0, [WIN])
    itp.allocate_tensors()
    itp.set_tensor(itp.get_input_details()[0]['index'], x.astype(np.float32))
    itp.invoke()
    ref = next(itp.get_tensor(d['index']).reshape(-1)
               for d in itp.get_output_details() if d['shape'][-1] == 521)
    delta = np.abs(pred - ref).max()
    print(f'weights check: max|keras-tflite| = {delta:.4f} '
          f'(snore keras={pred[SNORE_IDX]:.3f} tflite={ref[SNORE_IDX]:.3f})')
    return delta < 0.05


def main():
    tf, params_mod, yamnet_mod = _import_tf()
    make_wavs()
    wavs = sorted(glob.glob(os.path.join(CALIB_WAV, '*.wav')))
    print(f'{len(wavs)} calib wavs')
    patches, _labels = build_patches(wavs)

    p = params_mod.Params(sample_rate=SR, patch_hop_seconds=0.48)

    # yamnet.h5 stores the WHOLE frames-model weights in file order, so load
    # them into the frames model WITHOUT by_name, then transfer per-layer
    # into a freshly built mel-patch model (same layer names - yamnet() is
    # the shared builder). by_name against this h5 fails silently and yields
    # a random-weight model (the released chayuto int8 model is exactly that
    # mistake - it outputs ~0.5 for every input).
    frames = yamnet_mod.yamnet_frames_model(p)
    frames.load_weights(os.path.join(MCU, 'yamnet.h5'))
    wdict = {layer.name: layer.get_weights() for layer in frames.layers
             if layer.get_weights()}
    n_w = sum(len(v) for v in wdict.values())
    print(f'frames model loaded: {len(wdict)} layers, {n_w} weight tensors')
    tf.keras.backend.clear_session()

    mel_input = tf.keras.Input(batch_size=1, shape=(p.patch_frames, p.patch_bands),
                               dtype=tf.float32, name='mel_patch')
    predictions, _ = yamnet_mod.yamnet(mel_input, p)
    model = tf.keras.Model(inputs=mel_input, outputs=predictions,
                           name='yamnet_mel_patch')
    n_set = 0
    for layer in model.layers:
        if layer.name in wdict and layer.get_weights():
            try:
                layer.set_weights(wdict[layer.name])
                n_set += 1
            except Exception as e:
                print(f'  transfer failed for {layer.name}: {e}')
    print(f'transferred weights into {n_set}/{len(wdict)} layers')
    if not verify_weights(model):
        print('FATAL: transferred weights do not match float tflite - aborting')
        return 1

    def rep():
        for i in range(len(patches)):
            yield [patches[i][np.newaxis]]

    print('converting (int8 PTQ)...')
    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tflite = conv.convert()
    with open(OUT_MODEL, 'wb') as f:
        f.write(tflite)
    print(f'wrote {OUT_MODEL}: {len(tflite)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
