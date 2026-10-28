"""Shared exact-target build options and resource feature metadata."""

from pathlib import Path
import shutil

P67_TARGET = 'xiaomi-band-10-pro-3.101.043'
P65_TARGET = 'redmi-watch-6-3.100.043'


def stage_resources(root, project, manifest, targets):
    """Preserve manifest-relative resources in isolated packaging projects."""
    resources = manifest.get('resources', {})
    files = list(resources.get('files', []))
    for target in targets:
        files.extend(resources.get('targets', {}).get(target, []))
    for relative in files:
        destination = Path(project) / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(Path(root) / relative, destination)


def build_options(targets, environ):
    """One complete product per target; only P67 diagnostics is optional."""
    for legacy in ('RH_P67_LIVE_LAUNCHER', 'RH_P67_LIVE_BACKGROUNDS', 'RH_P67_BLUR_NATIVE'):
        if legacy in environ:
            raise ValueError(f'Retired build flag {legacy}; remove it. P67 backgrounds and native blur are now default.')
    diagnostics = environ.get('RH_P67_BLUR_DIAGNOSTICS', '0') == '1'
    if diagnostics and P67_TARGET not in targets:
        raise ValueError('Blur diagnostics requires exact .043 target in the bundle')
    return diagnostics


def target_build_environment(target, environ):
    """Do not leak P67-only diagnostics into another target in a mixed bundle."""
    env = dict(environ)
    if target != P67_TARGET:
        env.pop('RH_P67_BLUR_DIAGNOSTICS', None)
    return env


def background_metadata(targets, diagnostics=False):
    surfaces = ['launcher', 'control_center', 'notifications']
    p67 = P67_TARGET in targets
    if p67 or P65_TARGET in targets:
        surfaces.append('widgets')
    return {
        'background_targets': surfaces,
        'background_blur_mode': ('active_module_owned_snapshot_static_rgb565' if surfaces else 'UNSUPPORTED'),
        'background_blur_device_status': 'NOT_PROBED' if surfaces else 'UNSUPPORTED',
        'background_device_status': 'NOT_PROBED' if surfaces else 'UNSUPPORTED',
        **({'p67_blur_diagnostics': diagnostics} if p67 else {}),
    }


def feature_metadata(targets):
    """Exact-target support; aggregate font support requires every target."""
    features = {
        target: {
            'font_reload': True,
            'native_backgrounds': True,
            'calendar': True,
            'device_status': 'NOT_PROBED',
        }
        for target in targets
    }
    font_reload = all(features[target]['font_reload'] for target in targets)
    return {
        'target_features': features,
        'font_reload': font_reload,
        'font_reload_device_status': ('UNSUPPORTED' if not font_reload else
            'NOT_PROBED' if P65_TARGET in targets or any(target.endswith(('4.100.177', '4.100.179')) for target in targets)
            else 'USER_REPORTED_PASS'),
    }
