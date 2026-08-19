import unittest

import numpy as np

from motor_dsp import extract_features


class MotorDspTests(unittest.TestCase):
    def test_detects_known_sine_frequency_and_amplitude(self):
        sample_rate = 1024
        time = np.arange(1024) / sample_rate
        samples = 0.4 * np.sin(2 * np.pi * 50 * time)
        result = extract_features(samples, sample_rate, rpm=1500)
        self.assertAlmostEqual(result["dominantHz"], 50.0, delta=result["binWidthHz"])
        self.assertAlmostEqual(result["rmsG"], 0.4 / np.sqrt(2), places=3)
        self.assertAlmostEqual(result["dominantAmplitudeG"], 0.4, places=3)
        self.assertEqual(result["shaftHz"], 25.0)

    def test_rejects_non_power_of_two_input(self):
        with self.assertRaisesRegex(ValueError, "power of two"):
            extract_features([1, 2, 3] * 25, 1000)

    def test_zero_signal_is_finite(self):
        result = extract_features(np.zeros(256), 512)
        self.assertEqual(result["rmsG"], 0)
        self.assertEqual(result["crestFactor"], 0)


if __name__ == "__main__":
    unittest.main()

