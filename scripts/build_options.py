"""Shared exact-target build options and resource feature metadata."""

P67_TARGET = 'xiaomi-band-10-pro-3.101.043'


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
    if p67:
        surfaces.append('widgets')
    return {
        'background_targets': surfaces,
        'background_blur_mode': 'active_module_owned_snapshot_static_rgb565',
        'background_blur_device_status': 'NOT_PROBED',
        'background_device_status': 'NOT_PROBED',
        **({'p67_blur_diagnostics': diagnostics} if p67 else {}),
    }
