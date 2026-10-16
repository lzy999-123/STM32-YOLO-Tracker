import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / 'tools' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


trace = module('analyze_research_trace')
calibrate = module('calibrate_research_camera')


def records(events):
    rows = [dict(kind='run_start', clock='host_steady_clock_us', host_mono_us=0)]
    rows += events
    rows += [dict(kind='run_end', host_mono_us=1000000, complete=True, dropped_events=0)]
    for i, row in enumerate(rows):
        row.update(schema_version=1, run_id='synthetic-unit-test-only', event_seq=i)
    return rows


def observation(**changes):
    row = dict(kind='observation', stream_id=2, frame_id=1, dnn_session=3,
        host_mono_us=31000, host_receive_us=20000, inference_start_us=22000,
        inference_done_us=30000, outcome='detected', fresh_measurement=True,
        session_current_at_completion=True, width=1280, height=720, rotation=3,
        raw_x=630, raw_y=350, raw_w=20, raw_h=20, confidence=.9)
    row.update(changes)
    return row


def frame():
    return dict(kind='frame', stream_id=2, frame_id=1, host_mono_us=20000,
                host_receive_us=20000, width=1280,height=720,rotation=3)


def telemetry(**changes):
    row = dict(kind='telemetry', host_mono_us=11000, host_receive_us=10000,
               horizontal_deg=20, vertical_deg=0, flags=0, calibration_valid=True,
               connection_id=1, telemetry_seq=1)
    row.update(changes)
    return row


def configuration():
    return dict(schema_version=1, reviewed_for_diagnostics=True, horizontal_sign=1,
        encoder_zero_deg=0, rotation=3, source_images=[{'sha256':'synthetic-unit-test-only'}],
        camera_matrix=[[800,0,640],[0,800,360],[0,0,1]],
        distortion_coefficients=[0]*5, image_size=[1280,720])


class ResearchToolsTest(unittest.TestCase):
    def test_host_trace_never_claims_synchronized_acceptance(self):
        result = trace.audit(records([telemetry(),frame(),observation()]))
        self.assertTrue(result['trace_integrity_pass'])
        self.assertFalse(result['synchronized_los_ready'])
        self.assertIsNone(result['optical_end_to_end_latency_ms'])
        self.assertEqual(result['preprocess_inference_postprocess_ms']['maximum'],8)

    def test_missing_footer_rejected(self):
        self.assertFalse(trace.audit(records([])[:-1])['trace_integrity_pass'])

    def test_overflow_rejected(self):
        rows=records([]); rows[-1]['dropped_events']=2
        self.assertFalse(trace.audit(rows)['trace_integrity_pass'])

    def test_mixed_runs_rejected(self):
        rows=records([frame()]); rows[1]['run_id']='other'
        self.assertFalse(trace.audit(rows)['trace_integrity_pass'])

    def test_missing_or_wrong_frame_rejected(self):
        self.assertFalse(trace.audit(records([observation()]))['trace_integrity_pass'])
        self.assertFalse(trace.audit(records([frame(),observation(frame_id=2)]))['trace_integrity_pass'])

    def test_duplicate_observation_rejected(self):
        self.assertFalse(trace.audit(records([frame(),observation(),observation()]))['trace_integrity_pass'])

    def test_hold_cannot_be_fresh(self):
        result=trace.audit(records([frame(),observation(outcome='hold')]))
        self.assertFalse(result['trace_integrity_pass'])
        result=trace.audit(records([frame(),observation(outcome='hold',fresh_measurement=False)]))
        self.assertTrue(result['trace_integrity_pass'])
        self.assertEqual(result['fresh_detection_count'],0)

    def test_reversed_pipeline_times_rejected(self):
        self.assertFalse(trace.audit(records([frame(),observation(inference_start_us=32000)]))['trace_integrity_pass'])

    def test_no_future_telemetry_join(self):
        rows=records([frame(),telemetry(host_mono_us=26000,host_receive_us=25000),observation()])
        result=trace.approximate_los(rows,configuration(),100)
        self.assertEqual(result['samples'],[])
        self.assertEqual(result['rejected']['no_prior_telemetry'],1)

    def test_causal_center_projection(self):
        result=trace.approximate_los(records([telemetry(),frame(),observation()]),configuration(),100)
        self.assertEqual(result['samples'][0]['approx_los_deg'],20)
        self.assertEqual(result['samples'][0]['encoder_event_seq'],1)
        self.assertFalse(result['acceptance_pass'])

    def test_stale_and_fault_rejected(self):
        result=trace.approximate_los(records([telemetry(),frame(),observation()]),configuration(),5)
        self.assertEqual(result['rejected']['stale_telemetry'],1)
        result=trace.approximate_los(records([telemetry(flags=1),frame(),observation()]),configuration(),100)
        self.assertEqual(result['rejected']['fault_or_session_boundary'],1)

    def test_reconnection_between_frame_and_result_rejected(self):
        rows=records([telemetry(),frame(),telemetry(host_mono_us=26000,host_receive_us=25000,connection_id=2),observation()])
        self.assertEqual(trace.approximate_los(rows,configuration(),100)['samples'],[])

    def test_geometry_mismatch_rejected(self):
        result=trace.approximate_los(records([telemetry(),frame(),observation(rotation=0)]),configuration(),100)
        self.assertEqual(result['rejected']['geometry_mismatch'],1)

    def test_unreviewed_or_invalid_calibration_refused(self):
        rows=records([telemetry(),frame(),observation()])
        for key,value in [('reviewed_for_diagnostics',False),('horizontal_sign',0),('encoder_zero_deg',None),('source_images',[])]:
            config=configuration(); config[key]=value
            with self.assertRaises(ValueError): trace.approximate_los(rows,config,100)

    def test_raw_hash_and_malformed_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'trace.jsonl'
            path.write_text('\n'.join(json.dumps(x) for x in records([]))+'\n',encoding='utf-8')
            loaded,digest=trace.load_trace(path)
            self.assertEqual(len(digest),64)
            self.assertTrue(trace.audit(loaded)['trace_integrity_pass'])
            path.write_text('{broken',encoding='utf-8')
            with self.assertRaises(ValueError): trace.load_trace(path)

    def test_synthetic_calibration_math_not_physical_evidence(self):
        import cv2
        import numpy as np
        template=np.zeros((9*6,3),np.float32)
        template[:,:2]=np.mgrid[0:9,0:6].T.reshape(-1,2)*20
        matrix=np.array([[850.,0,640],[0,860,360],[0,0,1]])
        objects,points=[],[]
        for i in range(20):
            rotation=np.array([.05+(i%5)*.08, -.3+(i//5)*.15, .02*i])
            translation=np.array([-100.+(i%4)*25,-70.+(i%3)*20,600.+i*13])
            projected,_=cv2.projectPoints(template,rotation,translation,matrix,np.zeros(5))
            objects.append(template.copy()); points.append(projected)
        result=calibrate.fit_points(objects,points,(1280,720))
        self.assertLess(abs(result['camera_matrix'][0][0]-850),1)
        self.assertLess(max(result['heldout_pose_fitted_rms_px']),.01)
        self.assertTrue(set(result['training_indices']).isdisjoint(result['heldout_indices']))
        with self.assertRaises(ValueError): calibrate.fit_points(objects[:5],points[:5],(1280,720))

    def test_duplicate_calibration_images_rejected(self):
        import cv2
        import numpy as np
        with tempfile.TemporaryDirectory() as directory:
            first=Path(directory)/'first.png'; second=Path(directory)/'second.png'
            _,image=cv2.imencode('.png',np.zeros((32,32),np.uint8))
            first.write_bytes(image.tobytes()); second.write_bytes(image.tobytes())
            with self.assertRaisesRegex(ValueError,'Duplicate'):
                calibrate.calibrate([first,second],3,3,20,3)

    def test_mixed_calibration_resolution_rejected(self):
        import cv2
        import numpy as np
        with tempfile.TemporaryDirectory() as directory:
            paths=[]
            for index,size in enumerate([32,48]):
                path=Path(directory)/f'{index}.png'
                _,image=cv2.imencode('.png',np.zeros((size,size),np.uint8))
                path.write_bytes(image.tobytes()); paths.append(path)
            with self.assertRaisesRegex(ValueError,'Mixed'):
                calibrate.calibrate(paths,3,3,20,3)

    def test_nonfinite_calibration_and_box_rejected(self):
        rows=records([telemetry(),frame(),observation()])
        config=configuration(); config['camera_matrix'][0][0]=float('nan')
        with self.assertRaises(ValueError): trace.approximate_los(rows,config,100)
        result=trace.approximate_los(records([telemetry(),frame(),observation(raw_x=float('nan'))]),configuration(),100)
        self.assertEqual(result['rejected']['invalid_box'],1)

    def test_cli_refuses_existing_output(self):
        import subprocess
        import sys
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'trace.jsonl'; output=Path(directory)/'result.json'
            path.write_text('\n'.join(json.dumps(x) for x in records([]))+'\n',encoding='utf-8')
            output.write_text('preserve-this',encoding='utf-8')
            command=[sys.executable,trace.__file__,str(path),'--output',str(output)]
            result=subprocess.run(command,capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertEqual(output.read_text(encoding='utf-8'),'preserve-this')


if __name__ == '__main__':
    unittest.main()
