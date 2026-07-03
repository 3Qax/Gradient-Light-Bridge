class ColorXY {
  readonly x: number;
  readonly y: number;

  constructor(x: number, y: number) {
    this.x = x;
    this.y = y;
  }

  toScaledGradient(): Uint8Array {
    const maxX = 0.7347;
    const maxY = 0.8431;

    const x = this.x <= maxX ? Math.floor((this.x * 4095) / 0.7347) : 4095;
    const y = this.y <= maxY ? Math.floor((this.y * 4095) / 0.8413) : 4095;

    return new Uint8Array([
      x & 0x0ff,
      ((x & 0xff0) >> 8) | ((y & 0x00f) << 4),
      (y & 0xff0) >> 4,
    ]);
  }
}

function invGammaCorrection(value: number): number {
  if (value <= 0.0404482362771076) {
    return value / 12.92;
  }
  return Math.pow((value + 0.055) / 1.055, 2.4);
}

class ColorRGB {
  readonly r: number;
  readonly g: number;
  readonly b: number;

  constructor(r: number, g: number, b: number) {
    this.r = r;
    this.g = g;
    this.b = b;
  }

  withGammaCorrection(): ColorRGB {
    return new ColorRGB(
      Math.floor(invGammaCorrection(this.r)),
      Math.floor(invGammaCorrection(this.g)),
      Math.floor(invGammaCorrection(this.b)),
    );
  }

  static fromHex(value: string): ColorRGB {
    value = value.replace("#", "");
    return new ColorRGB(
      parseInt(value.slice(0, 2), 16),
      parseInt(value.slice(2, 4), 16),
      parseInt(value.slice(4, 6), 16),
    );
  }

  toXY(): ColorXY {
    const color = this.withGammaCorrection();
    const x = color.r * 0.664511 + color.g * 0.154324 + color.b * 0.162028;
    const y = color.r * 0.283881 + color.g * 0.668433 + color.b * 0.047685;
    const z = color.r * 0.000088 + color.g * 0.007231 + color.b * 0.986039;
    const sum = x + y + z;

    return new ColorXY(sum === 0 ? 0 : x / sum, sum === 0 ? 0 : y / sum);
  }
}

export enum GradientStyle {
  Linear = 0x00,
  Scattered = 0x02,
  Mirrored = 0x04,
}

export class GradientCommandBuilder {
  readonly rgbColors: string[];
  readonly transitionTime: number;
  readonly segments: number;
  readonly offset: number;
  readonly style: GradientStyle;

  constructor(
    rgbColors: string[],
    transitionTime: number,
    segments: number,
    offset: number,
    style: GradientStyle,
  ) {
    this.rgbColors = rgbColors;
    this.transitionTime = transitionTime;
    this.segments = segments;
    this.offset = offset;
    this.style = style;
  }

  serialize(): Uint8Array {
    const data: number[] = [];

    // Set gradient
    data.push(0x50, 0x01);

    // Transition time
    data.push(this.transitionTime, 0x00);

    // Number of colors
    const colorCount = this.rgbColors.length;
    data.push(1 + 3 * (colorCount + 1));
    data.push(colorCount << 4);

    // Style
    data.push(this.style);

    // Padding
    data.push(0x0, 0x0);

    for (const color of this.rgbColors) {
      const scaledGradient = ColorRGB.fromHex(color).toXY().toScaledGradient();
      data.push(...scaledGradient);
    }

    data.push(this.segments << 3);
    data.push(this.offset << 3);

    return new Uint8Array(data);
  }
}
