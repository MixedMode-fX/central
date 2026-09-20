// What a stored byte means, in the parameter's own terms. Zero means the
// descriptor's default everywhere (param.h), and a signed parameter keeps an
// int8 in the byte, so neither can be shown as the raw number.

import * as P from '../../protocol/generated.js';
import { noteName, PITCH_CLASSES } from '../../core/music.js';
import { effectiveValue } from '../../core/patch.js';

const signed = (value) => (value > 0 ? `+${value}` : String(value));

export function paramText(pd, stored) {
  const effective = effectiveValue(pd, stored);
  switch (pd.kind) {
    case P.ParamKind.PARAM_SIGNED: return signed(stored > 127 ? stored - 256 : stored);
    case P.ParamKind.PARAM_CENTRED: return signed(effective - P.PARAM_CENTRE);
    case P.ParamKind.PARAM_ENUM: return pd.options?.[effective - pd.min] ?? String(effective);
    case P.ParamKind.PARAM_BOOL: return stored ? 'on' : 'off';
    case P.ParamKind.PARAM_MILLIS: return `${effective} ms`;
    case P.ParamKind.PARAM_PERCENT: return `${effective} %`;
    case P.ParamKind.PARAM_PITCH: return `${noteName(effective)} (${effective})`;
    case P.ParamKind.PARAM_PITCH_CLASS: return PITCH_CLASSES[effective % 12];
    case P.ParamKind.PARAM_CHANNEL: return effective === 0 ? 'omni' : `ch ${effective}`;
    // Not 'omni': on an outlet zero is not every channel, it is whichever one
    // the note came in on (src/node/param.h).
    case P.ParamKind.PARAM_CHANNEL_OUT: return effective === 0 ? 'as played' : `ch ${effective}`;
    case P.ParamKind.PARAM_BITFIELD: return `0b${effective.toString(2).padStart(8, '0')}`;
    default: return String(effective);
  }
}
