#!/usr/bin/env python3
"""Host-side validation + C header generation for M55 snore deployment.

1. Load int8 model (chayuto yamnet-mel-int8) and float waveform YAMNet (reference).
2. Rebuild the Google YAMNet frontend in numpy using constants extracted from the
   float model itself (const_400.npy = periodic Hann, const_16448.npy = mel matrix [64,257]).
3. Validate: numpy frontend -> quantize -> int8 scores  vs  float model scores
   on real audio windows (snore class 38 must track).
4. Generate: snore_model_data.c/.h (int8 tflite as C array) and
   snore_frontend_tables.h (hann[400] + mel[64][257]).

Run on the Pi:  ~/snore_venv/bin/python gen_headers.py [audio.mp3 ...]
"""
import subprocess, sys, os
import numpy as np

MCU = os.path.dirname(os.path.abspath(__file__))
SNORE_IDX = 38          # verified against yamnet_class_map.csv
SR = 16000
WIN = 15600             # 96 frames: 160*95 + 400
HOP = 7800
FFT = 512
FRAMES = 96

def _parse_args(argv):
    """argv: [--model path.tflite] [audio.mp3 ...]"""
    model = os.path.join(MCU, 'yamnet.tflite')
    rest = []
    i = 0
    while i < len(argv):
        if argv[i] == '--model' and i + 1 < len(argv):
            model = argv[i+1]; i += 2
        else:
            rest.append(argv[i]); i += 1
    return model, rest

def decode(path):
    """mp3/wav -> 16k mono float32 in [-1,1] via ffmpeg."""
    out = subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', path, '-ac', '1', '-ar', str(SR), '-f', 'f32le', '-'],
        capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype=np.float32)

def windows(x):
    n = 1 + (len(x) - WIN) // HOP if len(x) >= WIN else 0
    return [x[i*HOP:i*HOP+WIN] for i in range(n)]

def cfloat(v):
    """C float literal with 'f' suffix, always self-contained ('0f' is illegal)."""
    s = f'{v:.9g}'
    if 'e' not in s and '.' not in s and 'n' not in s:
        s += '.0'
    return s + 'f'

def logmel_patch(win16k, hann, mel):
    """win: float32[15600] -> log-mel patch float32[96,64]. Google spec:
    400-sample periodic Hann, 512-pt FFT, |X| (magnitude), mel matmul, log(x+0.001)."""
    f = 1 + (WIN - 400) // 160                      # 96 frames
    framed = np.lib.stride_tricks.as_strided(
        win16k, shape=(f, 400), strides=(win16k.strides[0]*160, win16k.strides[0]))
    spec = np.abs(np.fft.rfft(framed * hann, n=FFT, axis=1))   # [96,257]
    mel_sp = spec @ mel.T                                       # mel[64,257] -> [96,64]
    return np.log(mel_sp + 0.001).astype(np.float32)

def main():
    global SNORE_IDX
    from ai_edge_litert.interpreter import Interpreter

    int8_model, files = _parse_args(sys.argv[1:])
    if not files:
        files = [
            '/home/user/snore_check/audio/pos_180338.mp3',
            '/home/user/snore_check/audio/neg_ac_515859.mp3',
        ]
    float_model = '/home/user/snore_check/model/lite-model_yamnet_tflite_1.tflite'

    hann = np.load(os.path.join(MCU, 'const_400.npy')).reshape(-1)      # [400]
    mel = np.load(os.path.join(MCU, 'const_16448.npy')).astype(np.float32)  # [64,257]
    assert hann.shape == (400,) and mel.shape == (64, 257), (hann.shape, mel.shape)

    # ---- int8 interpreter ----
    itp8 = Interpreter(model_path=int8_model)
    itp8.allocate_tensors()
    inp = itp8.get_input_details()[0]
    out8 = itp8.get_output_details()[0]
    scale, zp = inp['quantization']
    print(f'int8 input: shape={inp["shape"]} scale={scale:.6f} zp={zp} '
          f'domain=[{( -128-zp)*scale:.2f},{(127-zp)*scale:.2f}]')

    # ---- float reference interpreter ----
    itpf = Interpreter(model_path=float_model)
    itpf.resize_tensor_input(0, [WIN])
    itpf.allocate_tensors()
    outsf = itpf.get_output_details()
    outf = next(d for d in outsf if d['shape'][-1] == 521)

    worst = 0.0
    for path in files:
        if not os.path.exists(path):
            print('missing', path); continue
        x = decode(path)
        for i, w in enumerate(windows(x)):
            patch = logmel_patch(w, hann, mel)
            q = np.clip(np.rint(patch / scale).astype(np.int32) + zp, -128, 127).astype(np.int8)
            itp8.set_tensor(inp['index'], q.reshape(1, FRAMES, 64))
            itp8.invoke()
            s8 = (itp8.get_tensor(out8['index']).reshape(-1).astype(np.int32)
                  - out8['quantization'][1]) * out8['quantization'][0]

            itpf.set_tensor(itpf.get_input_details()[0]['index'], w.astype(np.float32))
            itpf.invoke()
            sf = itpf.get_tensor(outf['index']).reshape(-1)

            d = abs(s8[SNORE_IDX] - sf[SNORE_IDX])
            worst = max(worst, d)
            top8 = int(np.argmax(s8)); topf = int(np.argmax(sf))
            print(f'{os.path.basename(path)} w{i}: int8 snore={s8[SNORE_IDX]:.3f} '
                  f'float snore={sf[SNORE_IDX]:.3f} |d|={d:.3f} '
                  f'top8={top8}({s8[top8]:.2f}) topf={topf}({sf[topf]:.2f})')
        print(f'  patch range: [{patch.min():.1f},{patch.max():.1f}]')
    print(f'WORST |int8-float| snore delta = {worst:.3f}')

    # ---- generate snore_model_data.c/.h ----
    mbytes = open(int8_model, 'rb').read()
    lines = []
    for i in range(0, len(mbytes), 16):
        lines.append(','.join(f'0x{b:02x}' for b in mbytes[i:i+16]) + ',')
    body = '\n'.join(lines)
    with open(os.path.join(MCU, 'snore_model_data.c'), 'w') as f:
        f.write('/* Auto-generated from yamnet.tflite '
                '(chayuto/yamnet-mel-int8-tflm) by gen_headers.py - do not edit. */\n'
                '#include "snore_model_data.h"\n\n'
                f'const unsigned char snore_model_data[{len(mbytes)}] '
                '__attribute__((aligned(16))) = {\n' + body + '\n};\n')
    with open(os.path.join(MCU, 'snore_model_data.h'), 'w') as f:
        f.write('/* Auto-generated - do not edit. */\n'
                '#ifndef SNORE_MODEL_DATA_H\n#define SNORE_MODEL_DATA_H\n\n'
                '#include <stddef.h>\n\n'
                '#ifdef __cplusplus\nextern "C" {\n#endif\n\n'
                f'#define SNORE_MODEL_SIZE {len(mbytes)}u\n'
                'extern const unsigned char snore_model_data[SNORE_MODEL_SIZE];\n\n'
                '#ifdef __cplusplus\n}\n#endif\n\n#endif\n')

    # ---- generate snore_frontend_tables.h ----
    def fmt(a):  # float32 full precision, 6 per row (handles any length)
        flat = np.asarray(a).reshape(-1)
        rows = [', '.join(cfloat(v) for v in flat[i:i+6])
                for i in range(0, flat.size, 6)]
        return ',\n'.join(rows)
    h = ('/* Auto-generated by gen_headers.py from constants extracted inside the\n'
         ' * float YAMNet tflite graph (tf_op_layer Mul_9/y = periodic Hann 400,\n'
         ' * MatMul_3 = linear-to-mel weight matrix [64,257]). Do not edit. */\n'
         '#ifndef SNORE_FRONTEND_TABLES_H\n#define SNORE_FRONTEND_TABLES_H\n\n'
         '#define SNORE_HANN_LEN 400\n'
         '#define SNORE_MEL_BANDS 64\n'
         '#define SNORE_SPEC_BINS 257\n'
         '#define SNORE_LOG_OFFSET 0.001f\n\n'
         'static const float snore_hann[SNORE_HANN_LEN] = {\n' + fmt(hann) + '\n};\n\n'
         'static const float snore_mel_matrix[SNORE_MEL_BANDS][SNORE_SPEC_BINS] = {\n'
         + ',\n'.join('{' + ', '.join(cfloat(v) for v in row) + '}'
                      for row in mel) + '\n};\n\n#endif\n')
    with open(os.path.join(MCU, 'snore_frontend_tables.h'), 'w') as f:
        f.write(h)

    for n in ('snore_model_data.c', 'snore_model_data.h', 'snore_frontend_tables.h'):
        p = os.path.join(MCU, n)
        print('wrote', p, os.path.getsize(p), 'bytes')

if __name__ == '__main__':
    main()
