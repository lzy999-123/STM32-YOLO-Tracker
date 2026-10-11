import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('cadence', Path(__file__).resolve().parents[1] / 'tools/analyze_video_cadence.py')
cadence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cadence)


def run_data(samples, seconds=60):
    return {'summary': {'seconds': seconds, 'frame_width': 1280, 'frame_height': 720},
            'submitted': samples, 'painted': samples}


def constant_frames():
    return [[(i + .5) * 1000 / 30, 2, i + 1, round(i * 1000000 / 30)] for i in range(1800)]


class VideoCadenceAnalysisTest(unittest.TestCase):
    def test_legacy_arrival_counts_do_not_certify_source_timing(self):
        data = run_data(constant_frames())
        data['summary']['source_pts_available'] = False
        for sample in data['painted']:
            sample[3] = -1
        result = cadence.analyze(data)
        self.assertEqual(result['painted']['per_second_distinct_frames'], [30] * 60)
        self.assertIsNone(result['painted']['source_missing_slots'])
        self.assertIsNone(result['painted']['source_nonmonotonic_steps'])
        self.assertFalse(result['continuous_cadence_pass'])

    def test_continuous_distinct_source_frames_pass(self):
        result = cadence.analyze(run_data(constant_frames()))
        self.assertTrue(result['continuous_cadence_pass'])
        self.assertEqual(result['painted']['per_second_distinct_frames'], [30] * 60)

    def test_fifteen_then_fortyfive_fails_despite_average_thirty(self):
        samples = []
        for second in range(60):
            count = 15 if second % 2 == 0 else 45
            for i in range(count):
                n = len(samples)
                samples.append([second * 1000 + (i + .5) * 1000 / count, 2, n + 1, round(n * 1000000 / 30)])
        result = cadence.analyze(run_data(samples))
        self.assertEqual(len(samples) / 60, 30)
        self.assertFalse(result['continuous_cadence_pass'])
        self.assertEqual(result['painted']['minimum_per_second'], 15)
        self.assertEqual(result['painted']['maximum_per_second'], 45)

    def test_repeated_paint_of_old_frame_is_not_a_new_frame(self):
        samples = [[(i + .5) * 1000 / 30, 2, i // 2 + 1, round((i // 2) * 1000000 / 30)] for i in range(1800)]
        result = cadence.analyze(run_data(samples))
        self.assertFalse(result['continuous_cadence_pass'])
        self.assertEqual(result['painted']['distinct_frames'], 900)

    def test_reconnect_trailing_outage_and_source_gap_fail(self):
        data = run_data(constant_frames())
        data['summary']['camera_states'] = [[-2000, 1], [-1500, 2], [30000, 4], [31000, 2]]
        self.assertFalse(cadence.analyze(data)['continuous_cadence_pass'])
        data = run_data(constant_frames()[:-2])
        self.assertFalse(cadence.analyze(data)['continuous_cadence_pass'])
        data = run_data(constant_frames())
        for frame in data['painted'][900:]:
            frame[3] += 33333
        self.assertFalse(cadence.analyze(data)['continuous_cadence_pass'])


if __name__ == '__main__':
    unittest.main()
