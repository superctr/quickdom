// Copyright 2026 Ian Karlsson
//! \file
//! The CSS properties the library computes (longhands), and the shorthands it expands into them. A property not
//! listed is ignored, as a browser ignores one it does not know.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qdom
{

//! X(id, name, inherited)
#define QDOM_PROPERTIES(X) \
	X(Display, "display", false) \
	X(Position, "position", false) \
	X(BoxSizing, "box-sizing", false) \
	X(Width, "width", false) \
	X(Height, "height", false) \
	X(MinWidth, "min-width", false) \
	X(MinHeight, "min-height", false) \
	X(MaxWidth, "max-width", false) \
	X(MaxHeight, "max-height", false) \
	X(MarginTop, "margin-top", false) \
	X(MarginRight, "margin-right", false) \
	X(MarginBottom, "margin-bottom", false) \
	X(MarginLeft, "margin-left", false) \
	X(PaddingTop, "padding-top", false) \
	X(PaddingRight, "padding-right", false) \
	X(PaddingBottom, "padding-bottom", false) \
	X(PaddingLeft, "padding-left", false) \
	X(BorderTopWidth, "border-top-width", false) \
	X(BorderRightWidth, "border-right-width", false) \
	X(BorderBottomWidth, "border-bottom-width", false) \
	X(BorderLeftWidth, "border-left-width", false) \
	X(BorderTopStyle, "border-top-style", false) \
	X(BorderRightStyle, "border-right-style", false) \
	X(BorderBottomStyle, "border-bottom-style", false) \
	X(BorderLeftStyle, "border-left-style", false) \
	X(BorderTopColor, "border-top-color", false) \
	X(BorderRightColor, "border-right-color", false) \
	X(BorderBottomColor, "border-bottom-color", false) \
	X(BorderLeftColor, "border-left-color", false) \
	X(BorderTopLeftRadius, "border-top-left-radius", false) \
	X(BorderTopRightRadius, "border-top-right-radius", false) \
	X(BorderBottomRightRadius, "border-bottom-right-radius", false) \
	X(BorderBottomLeftRadius, "border-bottom-left-radius", false) \
	X(Top, "top", false) \
	X(Right, "right", false) \
	X(Bottom, "bottom", false) \
	X(Left, "left", false) \
	X(FlexDirection, "flex-direction", false) \
	X(FlexWrap, "flex-wrap", false) \
	X(FlexGrow, "flex-grow", false) \
	X(FlexShrink, "flex-shrink", false) \
	X(FlexBasis, "flex-basis", false) \
	X(Order, "order", false) \
	X(AlignItems, "align-items", false) \
	X(AlignSelf, "align-self", false) \
	X(AlignContent, "align-content", false) \
	X(JustifyContent, "justify-content", false) \
	X(JustifyItems, "justify-items", false) \
	X(JustifySelf, "justify-self", false) \
	X(RowGap, "row-gap", false) \
	X(ColumnGap, "column-gap", false) \
	X(OverflowX, "overflow-x", false) \
	X(OverflowY, "overflow-y", false) \
	X(TextOverflow, "text-overflow", false) \
	X(TextDecorationLine, "text-decoration-line", false) \
	X(TextDecorationColor, "text-decoration-color", false) \
	X(WhiteSpace, "white-space", true) \
	X(TextAlign, "text-align", true) \
	X(TextTransform, "text-transform", true) \
	X(FontFamily, "font-family", true) \
	X(FontSize, "font-size", true) \
	X(FontWeight, "font-weight", true) \
	X(FontStyle, "font-style", true) \
	X(FontVariantNumeric, "font-variant-numeric", true) \
	X(LineHeight, "line-height", true) \
	X(LetterSpacing, "letter-spacing", true) \
	X(WordSpacing, "word-spacing", true) \
	X(VerticalAlign, "vertical-align", false) \
	X(Visibility, "visibility", true) \
	X(Color, "color", true) \
	X(BackgroundColor, "background-color", false) \
	X(BackgroundImage, "background-image", false) \
	X(Opacity, "opacity", false) \
	X(BoxShadow, "box-shadow", false) \
	X(OutlineWidth, "outline-width", false) \
	X(OutlineStyle, "outline-style", false) \
	X(OutlineColor, "outline-color", false) \
	X(OutlineOffset, "outline-offset", false) \
	X(Cursor, "cursor", true) \
	X(PointerEvents, "pointer-events", true) \
	X(UserSelect, "user-select", false) \
	X(ImageRendering, "image-rendering", true) \
	X(GridTemplateColumns, "grid-template-columns", false) \
	X(GridTemplateRows, "grid-template-rows", false) \
	X(GridAutoFlow, "grid-auto-flow", false) \
	X(GridAutoRows, "grid-auto-rows", false) \
	X(GridAutoColumns, "grid-auto-columns", false) \
	X(BorderSpacing, "border-spacing", true) \
	X(BorderCollapse, "border-collapse", true) \
	X(Content, "content", false) \
	X(Fill, "fill", true) \
	X(Stroke, "stroke", true) \
	X(ZIndex, "z-index", false) \
	X(ScrollbarWidth, "scrollbar-width", false) \
	X(ScrollbarGutter, "scrollbar-gutter", false) \
	X(ScrollbarColor, "scrollbar-color", true) \
	X(Resize, "resize", false) \
	X(TouchAction, "touch-action", false)

enum class Prop : uint16_t
{
#define QDOM_PROP_ENUM(id, name, inherited) id,
	QDOM_PROPERTIES(QDOM_PROP_ENUM)
#undef QDOM_PROP_ENUM
	Count
};

constexpr size_t PROP_COUNT = size_t(Prop::Count);

//! A property's name as CSS writes it ("min-width").
const char *prop_name(Prop p);
//! Whether a property takes its parent's value unless it is set.
bool prop_inherited(Prop p);
//! A longhand by its name, dashed ("min-width") or camel-cased ("minWidth").
std::optional<Prop> find_prop(std::string_view name);
//! Whether a name is a shorthand this library expands.
bool is_shorthand(std::string_view name);
//! Every property name the style object of an element accepts: the longhands, then the shorthands.
const std::vector<std::string> &style_property_names();

//! A declaration as the longhands it sets (a longhand as itself); false for a property not known.
bool expand(std::string_view name, std::string_view value, std::vector<std::pair<Prop, std::string>> &out);

//! "min-width" from "minWidth"; a name with a dash or a custom property kept.
std::string dashed(std::string_view name);
//! "minWidth" from "min-width".
std::string camel(std::string_view name);

} // namespace qdom
