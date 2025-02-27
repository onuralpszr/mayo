/****************************************************************************
** Copyright (c) 2021, Fougue Ltd. <http://www.fougue.pro>
** All rights reserved.
** See license at https://github.com/fougue/mayo/blob/master/LICENSE.txt
****************************************************************************/

#include "io_system.h"

#include "document.h"
#include "io_parameters_provider.h"
#include "io_reader.h"
#include "io_writer.h"
#include "messenger.h"
#include "task_manager.h"
#include "task_progress.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <future>
#include <locale>
#include <mutex>
#include <regex>
#include <vector>

namespace Mayo {
namespace IO {

namespace {

TaskProgress* nullTaskProgress()
{
    static TaskProgress null;
    return &null;
}

Messenger* nullMessenger()
{
    return NullMessenger::instance();
}

bool containsFormat(Span<const Format> spanFormat, Format format)
{
    auto itFormat = std::find(spanFormat.begin(), spanFormat.end(), format);
    return itFormat != spanFormat.end();
}

} // namespace

void System::addFormatProbe(const FormatProbe& probe)
{
    m_vecFormatProbe.push_back(probe);
}

Format System::probeFormat(const FilePath& filepath) const
{
    std::ifstream file;
    file.open(filepath);
    if (file.is_open()) {
        std::array<char, 2048> buff;
        buff.fill(0);
        file.read(buff.data(), buff.size());
        FormatProbeInput probeInput = {};
        probeInput.filepath = filepath;
        probeInput.contentsBegin = QByteArray::fromRawData(buff.data(), buff.size());
        probeInput.hintFullSize = std::filesystem::file_size(filepath);
        for (const FormatProbe& fnProbe : m_vecFormatProbe) {
            const Format format = fnProbe(probeInput);
            if (format != Format_Unknown)
                return format;
        }
    }

    // Try to guess from file suffix
    std::string fileSuffix = filepath.extension().u8string();
    if (!fileSuffix.empty() && fileSuffix.front() == '.')
        fileSuffix.erase(fileSuffix.begin());

    auto fnCharIEqual = [](char lhs, char rhs) {
        const auto& clocale = std::locale::classic();
        return std::tolower(lhs, clocale) == std::tolower(rhs, clocale);
    };
    auto fnMatchFileSuffix = [=](Format format) {
        for (std::string_view candidate : formatFileSuffixes(format)) {
            if (candidate.size() == fileSuffix.size()
                    && std::equal(candidate.cbegin(), candidate.cend(), fileSuffix.cbegin(), fnCharIEqual))
            {
                return true;
            }
        }

        return false;
    };
    for (Format format : m_vecReaderFormat) {
        if (fnMatchFileSuffix(format))
            return format;
    }

    for (Format format : m_vecWriterFormat) {
        if (fnMatchFileSuffix(format))
            return format;
    }

    return Format_Unknown;
}

void System::addFactoryReader(std::unique_ptr<FactoryReader> ptr)
{
    if (!ptr)
        return;

    auto itFactory = std::find(m_vecFactoryReader.cbegin(), m_vecFactoryReader.cend(), ptr);
    if (itFactory != m_vecFactoryReader.cend())
        return;

    for (Format format : ptr->formats()) {
        auto itFormat = std::find(m_vecReaderFormat.cbegin(), m_vecReaderFormat.cend(), format);
        if (itFormat == m_vecReaderFormat.cend())
            m_vecReaderFormat.push_back(format);
    }

    m_vecFactoryReader.push_back(std::move(ptr));
}

void System::addFactoryWriter(std::unique_ptr<FactoryWriter> ptr)
{
    if (!ptr)
        return;

    auto itFactory = std::find(m_vecFactoryWriter.cbegin(), m_vecFactoryWriter.cend(), ptr);
    if (itFactory != m_vecFactoryWriter.cend())
        return;

    for (IO::Format format : ptr->formats()) {
        auto itFormat = std::find(m_vecWriterFormat.cbegin(), m_vecWriterFormat.cend(), format);
        if (itFormat == m_vecWriterFormat.cend())
            m_vecWriterFormat.push_back(format);
    }

    m_vecFactoryWriter.push_back(std::move(ptr));
}

const FactoryReader* System::findFactoryReader(Format format) const
{
    for (const std::unique_ptr<FactoryReader>& ptr : m_vecFactoryReader) {
        if (containsFormat(ptr->formats(), format))
            return ptr.get();
    }

    return nullptr;
}

const FactoryWriter* System::findFactoryWriter(Format format) const
{
    for (const std::unique_ptr<FactoryWriter>& ptr : m_vecFactoryWriter) {
        if (containsFormat(ptr->formats(), format))
            return ptr.get();
    }

    return nullptr;
}

std::unique_ptr<Reader> System::createReader(Format format) const
{
    const FactoryReader* ptr = this->findFactoryReader(format);
    if (ptr)
        return ptr->create(format);

    return {};
}

std::unique_ptr<Writer> System::createWriter(Format format) const
{
    const FactoryWriter* ptr = this->findFactoryWriter(format);
    if (ptr)
        return ptr->create(format);

    return {};
}

bool System::importInDocument(const Args_ImportInDocument& args)
{
    // NOTE
    // Maybe STEP/IGES CAF ReadFile() can be run concurrently(they should)
    // But concurrent calls to Transfer() to the same target Document must be serialized

    DocumentPtr doc = args.targetDocument;
    const auto listFilepath = args.filepaths;
    TaskProgress* rootProgress = args.progress ? args.progress : nullTaskProgress();
    Messenger* messenger = args.messenger ? args.messenger : nullMessenger();

    bool ok = true;

    using ReaderPtr = std::unique_ptr<Reader>;
    struct TaskData {
        ReaderPtr reader;
        FilePath filepath;
        Format fileFormat = Format_Unknown;
        TaskProgress* progress = nullptr;
        TaskId taskId = 0;
        TDF_LabelSequence seqTransferredEntity;
        bool readSuccess = false;
        bool transferred = false;
    };

    auto fnEntityPostProcessRequired = [&](Format format) {
        if (args.entityPostProcess && args.entityPostProcessRequiredIf)
            return args.entityPostProcessRequiredIf(format);
        else
            return false;
    };
    auto fnAddError = [&](const FilePath& fp, QString errorMsg) {
        ok = false;
        messenger->emitError(tr("Error during import of '%1'\n%2")
                             .arg(filepathTo<QString>(fp), errorMsg));
    };
    auto fnReadFileError = [&](const FilePath& fp, QString errorMsg) {
        fnAddError(fp, errorMsg);
        return false;
    };
    auto fnReadFile = [&](TaskData& taskData) {
        taskData.fileFormat = this->probeFormat(taskData.filepath);
        if (taskData.fileFormat == Format_Unknown)
            return fnReadFileError(taskData.filepath, tr("Unknown format"));

        int portionSize = 40;
        if (fnEntityPostProcessRequired(taskData.fileFormat))
            portionSize *= (100 - args.entityPostProcessProgressSize) / 100.;

        TaskProgress progress(taskData.progress, portionSize, tr("Reading file"));
        taskData.reader = this->createReader(taskData.fileFormat);
        if (!taskData.reader)
            return fnReadFileError(taskData.filepath, tr("No supporting reader"));

        taskData.reader->setMessenger(messenger);
        if (args.parametersProvider) {
            taskData.reader->applyProperties(
                        args.parametersProvider->findReaderParameters(taskData.fileFormat));
        }

        if (!taskData.reader->readFile(taskData.filepath, &progress))
            return fnReadFileError(taskData.filepath, tr("File read problem"));

        return true;
    };
    auto fnTransfer = [&](TaskData& taskData) {
        int portionSize = 60;
        if (fnEntityPostProcessRequired(taskData.fileFormat))
            portionSize *= (100 - args.entityPostProcessProgressSize) / 100.;

        TaskProgress progress(taskData.progress, portionSize, tr("Transferring file"));
        if (taskData.reader && !TaskProgress::isAbortRequested(&progress)) {
            taskData.seqTransferredEntity = taskData.reader->transfer(doc, &progress);
            if (taskData.seqTransferredEntity.IsEmpty())
                fnAddError(taskData.filepath, tr("File transfer problem"));
        }

        taskData.transferred = true;
    };
    auto fnPostProcess = [&](TaskData& taskData) {
        if (!fnEntityPostProcessRequired(taskData.fileFormat))
            return;

        TaskProgress progress(
                    taskData.progress,
                    args.entityPostProcessProgressSize,
                    args.entityPostProcessProgressStep);
        const double subPortionSize = 100. / double(taskData.seqTransferredEntity.Size());
        for (const TDF_Label& labelEntity : taskData.seqTransferredEntity) {
            TaskProgress subProgress(&progress, subPortionSize);
            args.entityPostProcess(labelEntity, &subProgress);
        }
    };
    auto fnAddModelTreeEntities = [&](TaskData& taskData) {
        for (const TDF_Label& labelEntity : taskData.seqTransferredEntity)
            doc->addEntityTreeNode(labelEntity);
    };

    if (listFilepath.size() == 1) { // Single file case
        TaskData taskData;
        taskData.filepath = listFilepath.front();
        taskData.progress = rootProgress;
        ok = fnReadFile(taskData);
        if (ok) {
            fnTransfer(taskData);
            fnPostProcess(taskData);
            fnAddModelTreeEntities(taskData);
        }
    }
    else { // Many files case
        std::vector<TaskData> vecTaskData;
        vecTaskData.resize(listFilepath.size());

        TaskManager childTaskManager;
        QObject::connect(&childTaskManager, &TaskManager::progressChanged, [&](TaskId, int) {
            rootProgress->setValue(childTaskManager.globalProgress());
        });

        // Read files
        for (TaskData& taskData : vecTaskData) {
            taskData.filepath = listFilepath[&taskData - &vecTaskData.front()];
            taskData.taskId = childTaskManager.newTask([&](TaskProgress* progressChild) {
                taskData.progress = progressChild;
                taskData.readSuccess = fnReadFile(taskData);
            });
        }

        for (const TaskData& taskData : vecTaskData)
            childTaskManager.run(taskData.taskId, TaskAutoDestroy::Off);

        // Transfer to document
        int taskDataCount = vecTaskData.size();
        while (taskDataCount > 0 && !rootProgress->isAbortRequested()) {
            auto it = std::find_if(vecTaskData.begin(), vecTaskData.end(), [&](const TaskData& taskData) {
                return !taskData.transferred && childTaskManager.waitForDone(taskData.taskId, 25);
            });

            if (it != vecTaskData.end()) {
                if (it->readSuccess) {
                    fnTransfer(*it);
                    fnPostProcess(*it);
                    fnAddModelTreeEntities(*it);
                }

                --taskDataCount;
            }
        } // endwhile
    }

    return ok;
}

System::Operation_ImportInDocument System::importInDocument() {
    return Operation_ImportInDocument(*this);
}

bool System::exportApplicationItems(const Args_ExportApplicationItems& args)
{
    TaskProgress* progress = args.progress ? args.progress : nullTaskProgress();
    Messenger* messenger = args.messenger ? args.messenger : nullMessenger();
    auto fnError = [=](const QString& errorMsg) {
        messenger->emitError(tr("Error during export to '%1'\n%2")
                             .arg(filepathTo<QString>(args.targetFilepath), errorMsg));
        return false;
    };

    std::unique_ptr<Writer> writer = this->createWriter(args.targetFormat);
    if (!writer)
        return fnError(tr("No supporting writer"));

    writer->setMessenger(args.messenger);
    writer->applyProperties(args.parameters);
    {
        TaskProgress transferProgress(progress, 40, tr("Transfer"));
        const bool okTransfer = writer->transfer(args.applicationItems, &transferProgress);
        if (!okTransfer)
            return fnError(tr("File transfer problem"));
    }

    {
        TaskProgress writeProgress(progress, 60, tr("Write"));
        const bool okWriteFile = writer->writeFile(args.targetFilepath, &writeProgress);
        if (!okWriteFile)
            return fnError(tr("File write problem"));
    }

    return true;
}

System::Operation_ExportApplicationItems&
System::Operation_ExportApplicationItems::targetFile(const FilePath& filepath) {
    m_args.targetFilepath = filepath;
    return *this;
}

System::Operation_ExportApplicationItems&
System::Operation_ExportApplicationItems::targetFormat(Format format) {
    m_args.targetFormat = format;
    return *this;
}

System::Operation_ExportApplicationItems&
System::Operation_ExportApplicationItems::withItems(Span<const ApplicationItem> appItems) {
    m_args.applicationItems = appItems;
    return *this;
}

System::Operation_ExportApplicationItems&
System::Operation_ExportApplicationItems::withParameters(const PropertyGroup* parameters) {
    m_args.parameters = parameters;
    return *this;
}

System::Operation_ExportApplicationItems&
System::Operation_ExportApplicationItems::withMessenger(Messenger* messenger) {
    m_args.messenger = messenger;
    return *this;
}

System::Operation_ExportApplicationItems&
System::Operation_ExportApplicationItems::withTaskProgress(TaskProgress* progress) {
    m_args.progress = progress;
    return *this;
}

bool System::Operation_ExportApplicationItems::execute() {
    return m_system.exportApplicationItems(m_args);
}

System::Operation_ExportApplicationItems::Operation_ExportApplicationItems(System& system)
    : m_system(system)
{
}

System::Operation_ExportApplicationItems System::exportApplicationItems()
{
    return Operation_ExportApplicationItems(*this);
}

System::Operation_ImportInDocument&
System::Operation_ImportInDocument::targetDocument(const DocumentPtr& document) {
    m_args.targetDocument = document;
    return *this;
}

System::Operation_ImportInDocument&
System::Operation_ImportInDocument::withFilepaths(Span<const FilePath> filepaths) {
    m_args.filepaths = filepaths;
    return *this;
}

System::Operation_ImportInDocument&
System::Operation_ImportInDocument::withParametersProvider(const ParametersProvider* provider) {
    m_args.parametersProvider = provider;
    return *this;
}

System::Operation_ImportInDocument&
System::Operation_ImportInDocument::withMessenger(Messenger* messenger) {
    m_args.messenger = messenger;
    return *this;
}

System::Operation_ImportInDocument&
System::Operation_ImportInDocument::withTaskProgress(TaskProgress* progress) {
    m_args.progress = progress;
    return *this;
}

System::Operation_ImportInDocument::Operation&
System::Operation_ImportInDocument::withFilepath(const FilePath& filepath)
{
    return this->withFilepaths(Span<const FilePath>(&filepath, 1));
}

System::Operation_ImportInDocument::Operation&
System::Operation_ImportInDocument::withEntityPostProcess(std::function<void (TDF_Label, TaskProgress*)> fn)
{
    m_args.entityPostProcess = std::move(fn);
    return *this;
}

System::Operation_ImportInDocument::Operation&
System::Operation_ImportInDocument::withEntityPostProcessRequiredIf(std::function<bool(Format)> fn)
{
    m_args.entityPostProcessRequiredIf = std::move(fn);
    return *this;
}

System::Operation_ImportInDocument::Operation&
System::Operation_ImportInDocument::withEntityPostProcessInfoProgress(int progressSize, const QString& progressStep)
{
    m_args.entityPostProcessProgressSize = progressSize;
    m_args.entityPostProcessProgressStep = progressStep;
    return *this;
}

bool System::Operation_ImportInDocument::execute() {
    return m_system.importInDocument(m_args);
}

System::Operation_ImportInDocument::Operation_ImportInDocument(System& system)
    : m_system(system)
{
}

namespace {

bool isSpace(char c) {
    return std::isspace(c, std::locale::classic());
}

bool matchToken(QByteArray::const_iterator itBegin, std::string_view token) {
    return std::strncmp(&(*itBegin), token.data(), token.size()) == 0;
}

auto findFirstNonSpace(const QByteArray& str) {
    return std::find_if_not(str.cbegin(), str.cend(), isSpace);
}

} // namespace

Format probeFormat_STEP(const System::FormatProbeInput& input)
{
    const QByteArray& sample = input.contentsBegin;
    // regex : ^\s*ISO-10303-21\s*;\s*HEADER
    constexpr std::string_view stepIsoId = "ISO-10303-21";
    constexpr std::string_view stepHeaderToken = "HEADER";
    auto itContentsBegin = findFirstNonSpace(sample);
    if (matchToken(itContentsBegin, stepIsoId)) {
        auto itChar = std::find_if_not(itContentsBegin + stepIsoId.size(), sample.cend(), isSpace);
        if (itChar != sample.cend() && *itChar == ';') {
            itChar = std::find_if_not(itChar + 1, sample.cend(), isSpace);
            if (matchToken(itChar, stepHeaderToken))
                return Format_STEP;
        }
    }

    return Format_Unknown;
}

Format probeFormat_IGES(const System::FormatProbeInput& input)
{
    const QByteArray& sample = input.contentsBegin;
    // regex : ^.{72}S\s*[0-9]+\s*[\n\r\f]
    bool isIges = true;
    if (sample.size() >= 80 && sample[72] == 'S') {
        for (int i = 73; i < 80 && isIges; ++i) {
            if (sample[i] != ' ' && !std::isdigit(static_cast<unsigned char>(sample[i])))
                isIges = false;
        }

        const char c80 = sample[80];
        if (isIges && (c80 == '\n' || c80 == '\r' || c80 == '\f')) {
            const int sVal = std::atoi(sample.data() + 73);
            if (sVal == 1)
                return Format_IGES;
        }
    }

    return Format_Unknown;
}

Format probeFormat_OCCBREP(const System::FormatProbeInput& input)
{
    // regex : ^\s*DBRep_DrawableShape
    auto itContentsBegin = findFirstNonSpace(input.contentsBegin);
    constexpr std::string_view occBRepToken = "DBRep_DrawableShape";
    if (matchToken(itContentsBegin, occBRepToken))
        return Format_OCCBREP;

    return Format_Unknown;
}

Format probeFormat_STL(const System::FormatProbeInput& input)
{
    const QByteArray& sample = input.contentsBegin;
    // Binary STL ?
    {
        constexpr size_t binaryStlHeaderSize = 80 + sizeof(uint32_t);
        if (sample.size() >= binaryStlHeaderSize) {
            constexpr uint32_t offset = 80; // Skip header
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(sample.data());
            const uint32_t facetsCount =
                    bytes[offset]
                    | (bytes[offset+1] << 8)
                    | (bytes[offset+2] << 16)
                    | (bytes[offset+3] << 24);
            constexpr unsigned facetSize = (sizeof(float) * 12) + sizeof(uint16_t);
            if ((facetSize * facetsCount + binaryStlHeaderSize) == input.hintFullSize)
                return Format_STL;
        }
    }

    // ASCII STL ?
    {
        // regex : ^\s*solid
        constexpr std::string_view asciiStlToken = "solid";
        auto itContentsBegin = findFirstNonSpace(input.contentsBegin);
        if (matchToken(itContentsBegin, asciiStlToken))
            return Format_STL;
    }

    return Format_Unknown;
}

Format probeFormat_OBJ(const System::FormatProbeInput& input)
{
    const QByteArray& sample = input.contentsBegin;
    const std::regex rx{ R"("^\s*(v|vt|vn|vp|surf)\s+[-\+]?[0-9\.]+\s")" };
    if (std::regex_search(sample.cbegin(), sample.cend(), rx))
        return Format_OBJ;

    return Format_Unknown;
}

void addPredefinedFormatProbes(System* system)
{
    if (!system)
        return;

    system->addFormatProbe(probeFormat_STEP);
    system->addFormatProbe(probeFormat_IGES);
    system->addFormatProbe(probeFormat_OCCBREP);
    system->addFormatProbe(probeFormat_STL);
    system->addFormatProbe(probeFormat_OBJ);
}

} // namespace IO
} // namespace Mayo
