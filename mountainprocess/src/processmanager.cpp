/******************************************************
** See the accompanying README and LICENSE files
** Author(s): Jeremy Magland
** Created: 4/27/2016
*******************************************************/

#include "processmanager.h"
#include <QDir>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QTime>
#include <QEventLoop>
#include <QCryptographicHash>
#include "mpdaemon.h"
#include "mlcommon.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <cachemanager.h>
#include "mpdaemon.h"

struct PMProcess {
    MLProcessInfo info;
    QProcess* qprocess;
};

class ProcessManagerPrivate {
public:
    ProcessManager* q;

    QMap<QString, MLProcessor> m_processors;
    QMap<QString, PMProcess> m_processes;
    //QStringList m_server_urls;
    //QString m_server_base_path;

    void clear_all_processes();
    void update_process_info(QString id);
    QString resolve_file_name_p(QString fname);
    QVariantMap resolve_file_names_in_parameters(QString processor_name, const QVariantMap& parameters);
    QString compute_unique_object_code(QJsonObject obj);
    QJsonObject compute_unique_process_object(MLProcessor P, const QVariantMap& parameters);
    bool all_input_and_output_files_exist(MLProcessor P, const QVariantMap& parameters);
    QJsonObject create_file_object(const QString& fname);

    static MLProcessor create_processor_from_json_object(QJsonObject obj);
    static MLParameter create_parameter_from_json_object(QJsonObject obj);
};

ProcessManager::ProcessManager()
{
    d = new ProcessManagerPrivate;
    d->q = this;

    QTimer::singleShot(1000, this, SLOT(slot_monitor()));
}

ProcessManager::~ProcessManager()
{
    d->clear_all_processes();
    delete d;
}

/*
void ProcessManager::setServerUrls(const QStringList& urls)
{
    d->m_server_urls = urls;
}

void ProcessManager::setServerBasePath(const QString& path)
{
    d->m_server_base_path = path;
}
*/

bool ProcessManager::loadProcessors(const QString& path, bool recursive)
{
    QStringList fnames = QDir(path).entryList(QStringList("*.mp"), QDir::Files, QDir::Name);
    foreach (QString fname, fnames) {
        if (!this->loadProcessorFile(path + "/" + fname))
            return false;
    }
    if (recursive) {
        QStringList subdirs = QDir(path).entryList(QStringList("*"), QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        foreach (QString subdir, subdirs) {
            if (!this->loadProcessors(path + "/" + subdir))
                return false;
        }
    }
    return true;
}

bool ProcessManager::loadProcessorFile(const QString& path)
{
    QString json;
    if (QFileInfo(path).isExecutable()) {
        QProcess pp;
        pp.start(path, QStringList("spec"));
        if (!pp.waitForFinished()) {
            qCritical() << "Problem with executable processor file, waiting for finish: " + path;
            return false;
        }
        pp.waitForReadyRead();
        QString output = pp.readAll();
        json = output;
        if (json.isEmpty()) {
            qWarning() << "Potential problem with executable processor file: " + path;
            if (QFileInfo(path).size() < 1e6) {
                json = TextFile::read(path);
                //now test it, since it is executable we are suspicious...
                QJsonParseError error;
                QJsonObject obj = QJsonDocument::fromJson(json.toLatin1(), &error).object();
                if (error.error != QJsonParseError::NoError) {
                    qCritical() << "Executable processor file did not return output for spec: " + path;
                    return false;
                }
                else {
                    //we are okay -- apparently the text file .mp got marked as executable by the user, so let's proceed
                }
            }
            else {
                qCritical() << "File is too large to be a text file. Executable processor file did not return output for spec: " + path;
                return false;
            }
        }
    }
    else {
        json = TextFile::read(path);
        if (json.isEmpty()) {
            qCritical() << "Processor file is empty: " + path;
            return false;
        }
    }
    QJsonParseError error;
    QJsonObject obj = QJsonDocument::fromJson(json.toLatin1(), &error).object();
    if (error.error != QJsonParseError::NoError) {
        qCritical() << "Json parse error: " << error.errorString();
        return false;
    }
    if (!obj["processors"].isArray()) {
        qCritical() << "Problem with processor file: processors field is missing or not any array: " + path;
        return false;
    }
    QJsonArray processors = obj["processors"].toArray();
    for (int i = 0; i < processors.count(); i++) {
        if (!processors[i].isObject()) {
            qCritical() << "Problem with processor file: processor is not an object: " + path;
            return false;
        }
        MLProcessor P = d->create_processor_from_json_object(processors[i].toObject());
        P.basepath = QFileInfo(path).path();
        if (P.name.isEmpty()) {
            qCritical() << "Problem with processor file: processor error: " + path;
            return false;
        }
        d->m_processors[P.name] = P;
    }
    return true;
}

QStringList ProcessManager::processorNames() const
{
    return d->m_processors.keys();
}

MLProcessor ProcessManager::processor(const QString& name)
{
    return d->m_processors.value(name);
}

QString ProcessManager::startProcess(const QString& processor_name, const QVariantMap& parameters_in, const RequestProcessResources& RPR)
{
    QVariantMap parameters = d->resolve_file_names_in_parameters(processor_name, parameters_in);

    if (!this->checkParameters(processor_name, parameters)) {
        qWarning() << "Problem checking parameters";
        return "";
    }

    if (!d->m_processors.contains(processor_name)) {
        qWarning() << "Unable to find processor (165): " + processor_name;
        return "";
    }
    MLProcessor P = d->m_processors[processor_name];

    QString exe_command = P.exe_command;
    exe_command.replace(QRegExp("\\$\\(basepath\\)"), P.basepath);
    {
        QString ppp;
        {
            QStringList keys = P.inputs.keys();
            foreach (QString key, keys) {
                QStringList list = MLUtil::toStringList(parameters[key]);
                exe_command.replace(QRegExp(QString("\\$%1\\$").arg(key)), list.value(0)); //note that only the first file name is inserted here
                foreach (QString str, list) {
                    ppp += QString("--%1=%2 ").arg(key).arg(str);
                }
            }
        }
        {
            QStringList keys = P.outputs.keys();
            foreach (QString key, keys) {
                QStringList list = MLUtil::toStringList(parameters[key]);
                exe_command.replace(QRegExp(QString("\\$%1\\$").arg(key)), list.value(0)); //note that only the first file name is inserted here
                foreach (QString str, list) {
                    ppp += QString("--%1=%2 ").arg(key).arg(str);
                }
            }
        }
        {
            QStringList keys = P.parameters.keys();
            foreach (QString key, keys) {
                exe_command.replace(QRegExp(QString("\\$%1\\$").arg(key)), parameters[key].toString());
                ppp += QString("--%1=%2 ").arg(key).arg(parameters[key].toString());
            }
        }

        if (RPR.request_num_threads)
            ppp += QString("--_request_num_threads=%1 ").arg(RPR.request_num_threads);

        exe_command.replace(QRegExp("\\$\\(arguments\\)"), ppp);
    }

    QString id = MLUtil::makeRandomId();
    PMProcess PP;
    PP.info.exe_command = exe_command;
    PP.info.parameters = parameters;
    PP.info.processor_name = processor_name;
    PP.info.finished = false;
    PP.info.exit_code = 0;
    PP.info.exit_status = QProcess::NormalExit;
    PP.qprocess = new QProcess;
    PP.qprocess->setProcessChannelMode(QProcess::MergedChannels);
    //connect(PP.qprocess,SIGNAL(readyRead()),this,SLOT(slot_qprocess_output()));
    QObject::connect(PP.qprocess, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(slot_process_finished()));
    printf("STARTING: %s.\n", PP.info.exe_command.toLatin1().data());
    PP.info.start_time = QDateTime::currentDateTime();

    // Do it this way so that special characters are handled exactly like a system call
    QString bash_script_fname = CacheManager::globalInstance()->makeLocalFile();
    TextFile::write(bash_script_fname, "#!/bin/bash\n" + PP.info.exe_command);
    PP.qprocess->start("/bin/bash", QStringList(bash_script_fname));
    //PP.qprocess->start(PP.info.exe_command);

    PP.qprocess->setProperty("pp_id", id);
    if (!PP.qprocess->waitForStarted(2000)) {
        qWarning() << "Problem starting process: " + exe_command;
        delete PP.qprocess;
        return "";
    }
    d->m_processes[id] = PP;
    return id;
}

bool ProcessManager::waitForFinished(const QString& process_id)
{
    if (!d->m_processes.contains(process_id))
        return false;
    QProcess* qprocess = d->m_processes[process_id].qprocess;
    return MPDaemon::waitForFinishedAndWriteOutput(qprocess);
}

bool ProcessManager::checkParameters(const QString& processor_name, const QVariantMap& parameters)
{
    if (processor_name.isEmpty())
        return true; //empty processor name is for handling .prv files
    if (!d->m_processors.contains(processor_name)) {
        qWarning() << "checkProcess: Unable to find processor (233): " + processor_name;
        return false;
    }
    if (processor_name.isEmpty())
        return true; //empty processor name is for handling .prv files
    MLProcessor P = d->m_processors[processor_name];
    {
        QStringList keys = P.inputs.keys();
        foreach (QString key, keys) {
            if (!parameters.contains(key)) {
                qWarning() << QString("checkProcess: Missing input in %1: %2").arg(processor_name).arg(key);
                return false;
            }
        }
    }
    {
        QStringList keys = P.outputs.keys();
        foreach (QString key, keys) {
            if (!parameters.contains(key)) {
                qWarning() << QString("checkProcess: Missing output in %1: %2").arg(processor_name).arg(key);
                return false;
            }
        }
    }
    {
        QStringList keys = P.parameters.keys();
        foreach (QString key, keys) {
            if (!P.parameters[key].optional) {
                if (!parameters.contains(key)) {
                    qWarning() << QString("checkProcess: Missing required parameter in %1: %2").arg(processor_name).arg(key);
                    return false;
                }
            }
        }
    }
    return true;
}

bool ProcessManager::processAlreadyCompleted(const QString& processor_name, const QVariantMap& parameters)
{
    if (!d->m_processors.contains(processor_name))
        return false;

    MLProcessor P = d->m_processors[processor_name];

    if (!d->all_input_and_output_files_exist(P, parameters))
        return false;

    QJsonObject obj = d->compute_unique_process_object(P, parameters);

    QString code = d->compute_unique_object_code(obj);

    QString path = MPDaemon::daemonPath() + "/completed_processes/" + code + ".json";

    return QFile::exists(path);
}

QStringList ProcessManager::allProcessIds() const
{
    return d->m_processes.keys();
}

void ProcessManager::clearProcess(const QString& id)
{
    if (!d->m_processes.contains(id))
        return;
    QProcess* qprocess = d->m_processes[id].qprocess;
    d->m_processes.remove(id);
    if (qprocess->state() == QProcess::Running) {
        d->m_processes[id].qprocess->kill();
        ;
    }
    delete qprocess;
}

void ProcessManager::clearAllProcesses()
{
    d->clear_all_processes();
}

MLProcessInfo ProcessManager::processInfo(const QString& id)
{
    if (!d->m_processes.contains(id)) {
        MLProcessInfo dummy;
        dummy.finished = true;
        dummy.exit_code = 0;
        dummy.exit_status = QProcess::NormalExit;
        return dummy;
    }
    d->update_process_info(id);
    return d->m_processes[id].info;
}

bool ProcessManager::isFinished(const QString& id)
{
    return processInfo(id).finished;
}

Q_GLOBAL_STATIC(ProcessManager, theInstance)
ProcessManager* ProcessManager::globalInstance()
{
    return theInstance;
}

void ProcessManager::slot_process_finished()
{
    QProcess* qprocess = qobject_cast<QProcess*>(sender());
    if (!qprocess) {
        qCritical() << "Unexpected problem in slot_process_finished: qprocess is null.";
        return;
    }
    {
        qprocess->waitForReadyRead();
        QByteArray str1 = qprocess->readAll();
        if (!str1.isEmpty()) {
            printf("%s", str1.data());
        }
    }
    QString id = qprocess->property("pp_id").toString();
    if (!d->m_processes.contains(id)) {
        qCritical() << "Unexpected problem in slot_process_finished. id not found in m_processes: " + id;
        return;
    }
    d->update_process_info(id);
    if ((qprocess->exitCode() == 0) && (qprocess->exitStatus() == QProcess::NormalExit)) {
        QString processor_name = d->m_processes[id].info.processor_name;
        QVariantMap parameters = d->m_processes[id].info.parameters;
        if (!d->m_processors.contains(processor_name)) {
            qCritical() << "Unexpected problem in slot_process_finished. processor not found!!! " + processor_name;
        }
        else {
            MLProcessor processor = d->m_processors[processor_name];
            QJsonObject obj = d->compute_unique_process_object(processor, parameters);
            QString code = d->compute_unique_object_code(obj);
            QString fname = MPDaemon::daemonPath() + "/completed_processes/" + code + ".json";
            QString json = QJsonDocument(obj).toJson();
            if (QFile::exists(fname))
                QFile::remove(fname); //shouldn't be needed
            if (TextFile::write(fname + ".tmp", json)) {
                QFile::rename(fname + ".tmp", fname);
                QFile::Permissions perm = QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser | QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
                QFile::setPermissions(fname, perm);
            }
        }
    }
    emit this->processFinished(id);
}

void ProcessManager::slot_qprocess_output()
{
    QProcess* P = qobject_cast<QProcess*>(sender());
    if (!P)
        return;
    QByteArray str = P->readAll();
    if (!str.isEmpty()) {
        printf("%s", str.data());
    }
}

QString execute_and_read_stdout(QString cmd)
{
    QProcess P;
    P.start(cmd);
    P.waitForStarted();
    P.waitForFinished();
    return P.readAllStandardOutput();
}

void ProcessManager::slot_monitor()
{
    QStringList ids = d->m_processes.keys();
    foreach (QString id, ids) {
        PMProcess* PP = &d->m_processes[id];
        if (PP->qprocess) {
            QString cmd = QString("ps -p %1 -o rss,%cpu --noheader").arg(PP->qprocess->pid());
            QString str = execute_and_read_stdout(cmd);
            QStringList list = str.split(" ", QString::SkipEmptyParts);
            MonitorStats MS;
            MS.timestamp = QDateTime::currentDateTime();
            MS.mem_bytes = list.value(0).toLong() * 1000;
            MS.cpu_pct = list.value(1).toDouble();
            PP->info.monitor_stats << MS;
        }
    }

    QTimer::singleShot(1000, this, SLOT(slot_monitor()));
}

void ProcessManagerPrivate::clear_all_processes()
{
    foreach (PMProcess P, m_processes) {
        delete P.qprocess;
    }
    m_processes.clear();
}

void ProcessManagerPrivate::update_process_info(QString id)
{
    if (!m_processes.contains(id))
        return;
    PMProcess* PP = &m_processes[id];
    QProcess* qprocess = PP->qprocess;
    if (qprocess->state() == QProcess::NotRunning) {
        PP->info.finish_time = QDateTime::currentDateTime();
        PP->info.finished = true;
        PP->info.exit_code = qprocess->exitCode();
        PP->info.exit_status = qprocess->exitStatus();
    }
    PP->info.standard_output += qprocess->readAll();
}

QString ProcessManagerPrivate::resolve_file_name_p(QString fname)
{
    //for now I have completely removed the client/server functionality

    if (fname.endsWith(".prv")) {
        QString txt = TextFile::read(fname);
        QJsonParseError err;
        QJsonObject obj = QJsonDocument::fromJson(txt.toUtf8(), &err).object();
        if (err.error != QJsonParseError::NoError) {
            qWarning() << "Error parsing .prv file: " + fname;
            return "";
        }
        QString path0 = locate_prv(obj);
        if (path0.isEmpty()) {
            qWarning() << "Unable to locate prv file originally at: " + obj["original_path"].toString();
            return "";
        }
        return path0;
    }
    else
        return fname;

    /*
    //This is terrible, we need to fix it!
    QString fname = fname_in;
    foreach (QString str, server_urls) {
        if (fname.startsWith(str + "/mdaserver")) {
            fname = server_base_path + "/" + fname.mid((str + "/mdaserver").count());
            if (fname.mid(server_base_path.count()).contains("..")) {
                qWarning() << "Illegal .. in file path: " + fname;
                fname = "";
            }
        }
    }
    */

    return fname;
}

QVariantMap ProcessManagerPrivate::resolve_file_names_in_parameters(QString processor_name, const QVariantMap& parameters_in)
{
    QVariantMap parameters = parameters_in;

    if (!m_processors.contains(processor_name)) {
        qWarning() << "Unable to find processor in resolve_file_names_in_parameters(): " + processor_name;
        return parameters_in;
    }
    MLProcessor MLP = m_processors[processor_name];
    foreach (MLParameter P, MLP.inputs) {
        QStringList list = MLUtil::toStringList(parameters[P.name]);
        if (list.count() == 1) {
            parameters[P.name] = resolve_file_name_p(list[0]);
        }
        else {
            QVariantList list2;
            foreach (QString str, list) {
                list2 << resolve_file_name_p(str);
            }
            parameters[P.name] = list2;
        }
    }
    foreach (MLParameter P, MLP.outputs) {
        QStringList list = MLUtil::toStringList(parameters[P.name]);
        if (list.count() == 1) {
            parameters[P.name] = resolve_file_name_p(list[0]);
        }
        else {
            QVariantList list2;
            foreach (QString str, list) {
                list2 << resolve_file_name_p(str);
            }
            parameters[P.name] = list2;
        }
    }
    return parameters;
}

MLProcessor ProcessManagerPrivate::create_processor_from_json_object(QJsonObject obj)
{
    MLProcessor P;
    P.spec = obj;
    P.name = obj["name"].toString();
    P.version = obj["version"].toString();
    P.description = obj["description"].toString();

    QJsonArray inputs = obj["inputs"].toArray();
    for (int i = 0; i < inputs.count(); i++) {
        MLParameter param = create_parameter_from_json_object(inputs[i].toObject());
        P.inputs[param.name] = param;
    }

    QJsonArray outputs = obj["outputs"].toArray();
    for (int i = 0; i < outputs.count(); i++) {
        MLParameter param = create_parameter_from_json_object(outputs[i].toObject());
        P.outputs[param.name] = param;
    }

    QJsonArray parameters = obj["parameters"].toArray();
    for (int i = 0; i < parameters.count(); i++) {
        MLParameter param = create_parameter_from_json_object(parameters[i].toObject());
        P.parameters[param.name] = param;
    }

    P.exe_command = obj["exe_command"].toString();

    return P;
}

MLParameter ProcessManagerPrivate::create_parameter_from_json_object(QJsonObject obj)
{
    MLParameter param;
    param.name = obj["name"].toString();
    param.ptype = obj["ptype"].toString();
    param.description = obj["description"].toString();
    param.optional = obj["optional"].toBool();
    param.default_value = obj["default_value"].toVariant();
    return param;
}

QJsonObject ProcessManagerPrivate::compute_unique_process_object(MLProcessor P, const QVariantMap& parameters)
{
    /*
     * Returns an object that depends uniquely on the following:
     *   1. Version of mountainprocess
     *   2. Processor name and version
     *   3. The paths, sizes, and modification times of the input files (together with their parameter names)
     *   4. Same for the output files
     *   5. The parameters converted to strings
     */

    QJsonObject obj;

    obj["mountainprocess_version"] = "0.1";
    obj["processor_name"] = P.name;
    obj["processor_version"] = P.version;
    {
        QJsonObject inputs;
        QStringList input_pnames = P.inputs.keys();
        qSort(input_pnames);
        foreach (QString input_pname, input_pnames) {
            QStringList fnames = MLUtil::toStringList(parameters[input_pname]);
            if (fnames.count() == 1) {
                inputs[input_pname] = create_file_object(resolve_file_name_p(fnames[0]));
            }
            else {
                QJsonArray array;
                foreach (QString fname0, fnames) {
                    QString fname = resolve_file_name_p(fname0);
                    array.append(create_file_object(fname));
                }
                inputs[input_pname] = array;
            }
        }
        obj["inputs"] = inputs;
    }
    {
        QJsonObject outputs;
        QStringList output_pnames = P.outputs.keys();
        qSort(output_pnames);
        foreach (QString output_pname, output_pnames) {
            QStringList fnames = MLUtil::toStringList(parameters[output_pname]);
            if (fnames.count() == 1) {
                outputs[output_pname] = create_file_object(resolve_file_name_p(fnames[0]));
            }
            else {
                QJsonArray array;
                foreach (QString fname0, fnames) {
                    QString fname = resolve_file_name_p(fname0);
                    array.append(create_file_object(fname));
                }
                outputs[output_pname] = array;
            }
        }
        obj["outputs"] = outputs;
    }
    {
        QJsonObject parameters0;
        QStringList pnames = P.parameters.keys();
        qSort(pnames);
        foreach (QString pname, pnames) {
            parameters0[pname] = parameters[pname].toString();
        }
        obj["parameters"] = parameters0;
    }
    return obj;
}

bool ProcessManagerPrivate::all_input_and_output_files_exist(MLProcessor P, const QVariantMap& parameters)
{
    QStringList file_pnames = P.inputs.keys();
    file_pnames.append(P.outputs.keys());
    foreach (QString pname, file_pnames) {
        QStringList fnames = MLUtil::toStringList(parameters.value(pname));
        foreach (QString fname0, fnames) {
            QString fname = resolve_file_name_p(fname0);
            if (!fname.isEmpty()) {
                if (!QFile::exists(fname))
                    return false;
            }
        }
    }
    return true;
}

QString ProcessManagerPrivate::compute_unique_object_code(QJsonObject obj)
{
    /// Witold I need a string that depends on the json object. However I am worried about the order of the fields. Is there a way to make this canonical?
    /// Jeremy: You can sort all keys in the dictionary, convert that to string and
    ///         calculate hash of that. However this is going to be CPU consuming
    QByteArray json = QJsonDocument(obj).toJson();
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(json);
    return QString(hash.result().toHex());
}

QJsonObject ProcessManagerPrivate::create_file_object(const QString& fname_in)
{
    QString fname = resolve_file_name_p(fname_in);
    QJsonObject obj;
    if (fname.isEmpty())
        return obj;
    obj["path"] = fname;
    if (!QFile::exists(fname)) {
        obj["size"] = 0;
        return obj;
    }
    obj["size"] = QFileInfo(fname).size();
    obj["last_modified"] = QFileInfo(fname).lastModified().toString("yyyy-MM-dd-hh-mm-ss-zzz");
    return obj;
}
