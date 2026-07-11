import unittest

import numpy as np

from python import analyze_feature_separability as analysis


class FeatureSeparabilityTests(unittest.TestCase):
    def test_candidate_morphology_features_are_shift_invariant_and_finite(self) -> None:
        timeline = np.arange(63, dtype=np.float32)
        window = np.zeros((63, 6), dtype=np.float32)
        window[:, 0] = 40.0 * np.sin(2.0 * np.pi * timeline / 21.0)
        window[:, 1] = 25.0 * np.cos(2.0 * np.pi * timeline / 21.0)
        window[:, 3] = 0.15 * np.sin(2.0 * np.pi * timeline / 21.0)
        window[:, 4] = 0.10 * np.cos(2.0 * np.pi * timeline / 21.0)
        window[:, 5] = 1.0 + 0.55 * np.sin(2.0 * np.pi * timeline / 21.0)

        original = analysis.candidate_morphology_features(window)
        shifted = analysis.candidate_morphology_features(np.roll(window, 7, axis=0))

        self.assertEqual(
            len(original), len(analysis.CANDIDATE_MORPHOLOGY_FEATURE_NAMES)
        )
        self.assertEqual(len(original), 40)
        self.assertTrue(np.isfinite(original).all())
        np.testing.assert_allclose(original, shifted, atol=2e-5, rtol=2e-5)

    def test_candidate_event_features_are_finite_and_complete(self) -> None:
        window = np.zeros((62, 6), dtype=np.float32)
        window[:, 0] = np.linspace(0.0, 100.0, 62)
        window[:, 5] = 1.0 + 0.5 * np.sin(np.linspace(0.0, 4.0 * np.pi, 62))

        features = analysis.candidate_event_features(window)

        self.assertEqual(len(features), len(analysis.CANDIDATE_EVENT_FEATURE_NAMES))
        self.assertEqual(len(features), 12)
        self.assertTrue(np.isfinite(features).all())

    def test_fisher_scores_rank_separated_feature_first(self) -> None:
        features = np.asarray(
            [
                [-2.0, 0.0],
                [-1.8, 1.0],
                [0.0, 0.0],
                [0.2, 1.0],
                [2.0, 0.0],
                [2.2, 1.0],
            ],
            dtype=np.float32,
        )
        labels = np.asarray([0, 0, 1, 1, 2, 2], dtype=np.int64)

        scores = analysis.fisher_scores(features, labels)

        self.assertGreater(scores[0], scores[1])
        self.assertTrue(np.isfinite(scores).all())

    def test_stable_pair_effect_requires_matching_direction(self) -> None:
        train_target = np.asarray([[3.0], [4.0]], dtype=np.float32)
        train_other = np.asarray([[0.0], [1.0]], dtype=np.float32)
        val_target = np.asarray([[2.5], [3.5]], dtype=np.float32)
        val_other = np.asarray([[0.2], [1.2]], dtype=np.float32)

        matching = analysis.stable_pair_effect(
            train_target, train_other, val_target, val_other
        )
        reversed_effect = analysis.stable_pair_effect(
            train_target, train_other, val_other, val_target
        )

        self.assertGreater(matching[0], 0.0)
        self.assertEqual(reversed_effect[0], 0.0)
        self.assertGreater(matching[1][0], 0.0)
        self.assertGreater(matching[2][0], 0.0)


if __name__ == "__main__":
    unittest.main()
