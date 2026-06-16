#pragma once

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxProperty.h"

namespace CorridorKey::OFX {

inline constexpr const char* kPluginIdentifier = "com.corridorkey.openfx";
inline constexpr const char* kPluginLabel = "CorridorKey OFX";
inline constexpr const char* kPluginGrouping = "Keyer";
inline constexpr unsigned int kPluginVersionMajor = 0;
inline constexpr unsigned int kPluginVersionMinor = 1;

void setHost(OfxHost* host);
OfxStatus mainEntry(const char* action,
                    const void* handle,
                    OfxPropertySetHandle inArgs,
                    OfxPropertySetHandle outArgs);

OfxStatus onLoad();
OfxStatus describe(OfxImageEffectHandle effect);
OfxStatus describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs);
OfxStatus defineParameters(OfxImageEffectHandle effect);
OfxStatus getClipPreferences(OfxImageEffectHandle instance,
                             OfxPropertySetHandle inArgs,
                             OfxPropertySetHandle outArgs);
OfxStatus render(OfxImageEffectHandle instance,
                 OfxPropertySetHandle inArgs,
                 OfxPropertySetHandle outArgs);
OfxStatus beginSequenceRender(OfxImageEffectHandle instance,
                              OfxPropertySetHandle inArgs,
                              OfxPropertySetHandle outArgs);
OfxStatus endSequenceRender(OfxImageEffectHandle instance,
                            OfxPropertySetHandle inArgs,
                            OfxPropertySetHandle outArgs);
OfxStatus instanceChanged(OfxImageEffectHandle instance,
                          OfxPropertySetHandle inArgs,
                          OfxPropertySetHandle outArgs);

OfxImageEffectSuiteV1* effectSuite();
OfxPropertySuiteV1* propertySuite();
OfxParameterSuiteV1* parameterSuite();

}  // namespace CorridorKey::OFX
