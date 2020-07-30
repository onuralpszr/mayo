/****************************************************************************
** Copyright (c) 2020, Fougue Ltd. <http://www.fougue.pro>
** All rights reserved.
** See license at https://github.com/fougue/mayo/blob/master/LICENSE.txt
****************************************************************************/

#pragma once

#include <fougtools/qttools/core/qstring_hfunc.h>
#include <QtCore/QLocale>
#include <QtCore/QObject>
#include <QtCore/QSettings>
#include <unordered_map>

#include "../base/unit_system.h"
#include "../base/string_utils.h"

#include <type_traits>

namespace Mayo {

class Property;

//class Module {
//public:
//    virtual void registerSettings() = 0;
//};

//class GuiModule {
//public:
//    static void init();
//};

template<typename SCALAR, typename TAG>
class TypedScalar {
public:
    using ScalarType = SCALAR;

    static_assert(std::is_scalar<SCALAR>::value, "Type T is not scalar");

    TypedScalar() = default;
    explicit TypedScalar(SCALAR scalar) : m_scalar(scalar) {}

    SCALAR get() const { return m_scalar; }

private:
    SCALAR m_scalar;
};

struct Settings_GroupTag {};
struct Settings_SectionTag {};
struct Settings_SettingTag {};
using Settings_GroupIndex = TypedScalar<int, Settings_GroupTag>;

class Settings_SectionIndex : public TypedScalar<int, Settings_SectionTag> {
public:
    Settings_SectionIndex() = default;
    explicit Settings_SectionIndex(Settings_GroupIndex group, ScalarType section)
        : m_group(group), m_section(section) {}

    Settings_GroupIndex group() const { return m_group; }

private:
    Settings_GroupIndex m_group;
    ScalarType m_section;
};

class Settings_SettingIndex : public TypedScalar<int, Settings_SettingTag> {
public:
    Settings_SettingIndex() = default;
    explicit Settings_SettingIndex(Settings_SectionIndex section, ScalarType setting)
        : m_section(section), m_setting(setting) {}

    Settings_GroupIndex group() const { return m_section.group(); }
    Settings_SectionIndex section() const { return m_section; }

private:
    Settings_SectionIndex m_section;
    ScalarType m_setting;
};

class Settings : public QObject {
    Q_OBJECT
public:
    using GroupIndex = Settings_GroupIndex;
    using SectionIndex = Settings_SectionIndex;
    using SettingIndex = Settings_SettingIndex;

    static Settings* instance();
    ~Settings();

    int groupCount() const;
    QByteArray groupId(GroupIndex index) const;
    QString groupTitle(GroupIndex index) const;
    void setGroupTitle(GroupIndex index, const QString& title) const;
    GroupIndex addGroup(QByteArray identifier);

    int sectionCount(GroupIndex index) const;
    QByteArray sectionIdentifier(SectionIndex index) const;
    QString sectionTitle(SectionIndex index) const;
    void setSectionTitle(SectionIndex index, const QString& title) const;
    bool isDefaultGroupSection(SectionIndex index) const;
    SectionIndex addSection(GroupIndex index, QByteArray identifier);

    int settingCount(SectionIndex index) const;
    Property* property(SettingIndex index) const;
    SettingIndex addSetting(Property* property, GroupIndex index);
    SettingIndex addSetting(Property* property, SectionIndex index);

    // Helpers

    const QLocale& locale() const;
    void setLocale(const QLocale& locale);

    QVariant value(const QString& key) const;
    template<typename T> T valueAs(const QString& key) const;
    template<typename ENUM> ENUM valueAsEnum(const QString& key) const;
    void setValue(const QString& key, const QVariant& value);

    const QVariant& defaultValue(const QString& key) const;
    void setDefaultValue(const QString& key, const QVariant& value);

    UnitSystem::Schema unitSystemSchema() const;
    int unitSystemDecimals() const;
    StringUtils::TextOptions defaultTextOptions() const;

signals:
    void valueChanged(const QString& key, const QVariant& value);

private:
    Settings();

    class Private;
    Private* const d = nullptr;
};



// --
// -- Implementation
// --

template<typename ENUM> ENUM Settings::valueAsEnum(const QString& key) const {
    // TODO Check returned value is QVariant-convertible to 'int'
    return static_cast<ENUM>(this->value(key).toInt());
}

template<typename T> T Settings::valueAs(const QString& key) const {
    // TODO Check returned value is QVariant-convertible to 'T'
    return this->value(key).value<T>();
}

} // namespace Mayo
