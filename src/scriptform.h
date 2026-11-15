#pragma once

#include <QJsonObject>
#include <QString>

// A form a script plugin puts to the reader (omabook.ui.form): a title, a
// few words to start with, and fields of six types. The script describes
// it; Omabook draws it, in its own style, and hands back what was filled in.
// Both ends go through here, so what a script is answered is always of the
// shape it asked for, whatever the window (or a test) sent:
//
//   {title, intro, accept,
//    fields: [{id, label, type, choices: [{value, label, help}], value,
//              min, max, step, help, placeholder, required, when}]}
//
// `type` is "text", "textarea", "choice" (one of the choices), "multi" (any
// number of them), "number" or "toggle". `when` shows a field only while
// other fields have certain values: {"where": ["sequel", "branch"]}.
namespace ScriptForm {

// The most fields a form may have, and choices a field may offer.
inline constexpr int mostFields = 40;
inline constexpr int mostChoices = 200;

// The form with everything filled in that may be left out (a choice given
// as a string, a field's starting value), or an empty object and `error`
// (English, for the script's author) when the form can't be drawn.
QJsonObject checked(const QJsonObject &form, QString *error);

// What a script is answered for a checked form: a value for every field,
// each of its type and within its bounds, from what was given; a field
// left out, or given something that doesn't fit, keeps its starting value.
QJsonObject values(const QJsonObject &form, const QJsonObject &given);

// Whether a field is shown with the form filled in as `values`.
bool shown(const QJsonObject &field, const QJsonObject &values);

// Why the form can't be accepted as filled in (a field that must be filled
// in and is shown, but empty), naming the field by its label; empty when it can.
QString missing(const QJsonObject &form, const QJsonObject &values);

} // namespace ScriptForm
