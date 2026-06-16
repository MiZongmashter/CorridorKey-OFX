# Color And Alpha Contract Fixtures

Numeric goldens live in `tests/cpp/test_color_alpha_contract.cpp`.

- Host Managed and Linear input modes pass RGB through as already scene-linear.
- sRGB-Rec709 Gamma uses the sRGB transfer curve to decode RGB to linear before
  any alpha multiplication.
- Alpha and matte values are always linear 0..1.
- Processed RGBA is linear premultiplied; Straight FG and Matte are linear
  unpremultiplied; status frames are opaque linear diagnostic images.
