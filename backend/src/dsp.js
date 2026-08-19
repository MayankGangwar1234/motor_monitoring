const isPowerOfTwo = (value) => value > 1 && (value & (value - 1)) === 0;

function fft(realInput) {
  const n = realInput.length;
  if (!isPowerOfTwo(n)) throw new Error("FFT sample count must be a power of two");

  const real = Float64Array.from(realInput);
  const imag = new Float64Array(n);

  for (let i = 1, j = 0; i < n; i += 1) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [real[i], real[j]] = [real[j], real[i]];
      [imag[i], imag[j]] = [imag[j], imag[i]];
    }
  }

  for (let length = 2; length <= n; length <<= 1) {
    const angle = (-2 * Math.PI) / length;
    const wLengthReal = Math.cos(angle);
    const wLengthImag = Math.sin(angle);
    for (let start = 0; start < n; start += length) {
      let wReal = 1;
      let wImag = 0;
      for (let offset = 0; offset < length / 2; offset += 1) {
        const even = start + offset;
        const odd = even + length / 2;
        const oddReal = real[odd] * wReal - imag[odd] * wImag;
        const oddImag = real[odd] * wImag + imag[odd] * wReal;
        const evenReal = real[even];
        const evenImag = imag[even];
        real[even] = evenReal + oddReal;
        imag[even] = evenImag + oddImag;
        real[odd] = evenReal - oddReal;
        imag[odd] = evenImag - oddImag;
        const nextReal = wReal * wLengthReal - wImag * wLengthImag;
        wImag = wReal * wLengthImag + wImag * wLengthReal;
        wReal = nextReal;
      }
    }
  }
  return { real, imag };
}

const round = (value, digits = 5) => Number(value.toFixed(digits));

function bandEnergy(spectrum, centreHz, halfWidthHz) {
  return round(spectrum
    .filter((bin) => Math.abs(bin.frequencyHz - centreHz) <= halfWidthHz)
    .reduce((sum, bin) => sum + bin.amplitudeG ** 2, 0));
}

export function analyzeVibration(samples, sampleRateHz, rpm = 0) {
  if (!Array.isArray(samples) || !isPowerOfTwo(samples.length)) {
    throw new Error("samples must be a power-of-two array");
  }
  if (!Number.isFinite(sampleRateHz) || sampleRateHz <= 0) {
    throw new Error("sampleRateHz must be positive");
  }
  if (!samples.every(Number.isFinite)) throw new Error("samples contain non-finite values");

  const n = samples.length;
  const mean = samples.reduce((sum, value) => sum + value, 0) / n;
  const centred = samples.map((value) => value - mean);
  const sumSquares = centred.reduce((sum, value) => sum + value * value, 0);
  const rmsG = Math.sqrt(sumSquares / n);
  const stdG = Math.sqrt(sumSquares / Math.max(n - 1, 1));
  const peakG = Math.max(...centred.map(Math.abs));
  const minG = Math.min(...centred);
  const maxG = Math.max(...centred);
  const window = centred.map((value, index) =>
    value * (0.5 - 0.5 * Math.cos((2 * Math.PI * index) / (n - 1)))
  );
  const windowSum = n > 1 ? (n - 1) / 2 : 1;
  const transformed = fft(window);
  const spectrum = [];
  for (let index = 0; index <= n / 2; index += 1) {
    const magnitude = Math.hypot(transformed.real[index], transformed.imag[index]);
    const scale = index === 0 || index === n / 2 ? 1 : 2;
    spectrum.push({
      frequencyHz: round((index * sampleRateHz) / n, 3),
      amplitudeG: round((scale * magnitude) / windowSum, 6)
    });
  }
  const dominant = spectrum.slice(1).reduce(
    (best, bin) => (bin.amplitudeG > best.amplitudeG ? bin : best),
    spectrum[1] || { frequencyHz: 0, amplitudeG: 0 }
  );
  const shaftHz = Number.isFinite(rpm) && rpm > 0 ? rpm / 60 : 0;
  const binWidthHz = sampleRateHz / n;

  return {
    sampleCount: n,
    sampleRateHz,
    binWidthHz: round(binWidthHz, 4),
    meanG: round(mean),
    rmsG: round(rmsG),
    stdG: round(stdG),
    peakG: round(peakG),
    peakToPeakG: round(maxG - minG),
    crestFactor: round(rmsG > 1e-12 ? peakG / rmsG : 0),
    dominantHz: dominant.frequencyHz,
    dominantAmplitudeG: dominant.amplitudeG,
    shaftHz: round(shaftHz, 3),
    oneXEnergy: shaftHz ? bandEnergy(spectrum, shaftHz, Math.max(binWidthHz * 1.5, 1)) : 0,
    twoXEnergy: shaftHz ? bandEnergy(spectrum, 2 * shaftHz, Math.max(binWidthHz * 1.5, 1)) : 0,
    spectrum
  };
}

export { fft };

