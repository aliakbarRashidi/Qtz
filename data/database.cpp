#include "database.h"
#include <agt/core/settings.h>
#include <agt/core/auth-provider.h>
#include <agt/core/qio.h>
#include <QFile>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QSqlError>
#include <QDate>
#include <QDateTime>
#include <QTime>
#include <QDataStream>
#include <QRegExp>
#include <string>

#include "table-node.h"

#include <QDebug>

using namespace std;

Database Database::instance;
bool Database::set = false;

void Database::setInstance(const QSqlDatabase &database, bool destroy)
{
    if(!set)
    {
        instance.m_database = database;
    }
    else if(destroy)
    {
        instance.m_database.close();
        instance.m_database = database;
    }
    set = true;
    getInstance()->setBlockSize(100);
}

Database *Database::getInstance()
{
    if(!set)
    {
        return NULL;
    }
    return &instance;
}

QSqlDatabase *Database::database()
{
    return &instance.m_database;
}

void Database::setType(const DatabaseType &newType)
{
    m_type = newType;
}

void Database::readConnectionInfo()
{
    QString driverName = Settings::getInstance()->value("type").toString();
    if(driverName==tr("MySQL"))
    {
        instance.m_database = QSqlDatabase::addDatabase("QMYSQL");
    }
    else if(driverName==tr("SQLite 3"))
    {
        instance.m_database = QSqlDatabase::addDatabase("QSQLITE");
    }
    instance.m_database.setHostName(Settings::getInstance()->value("host").toString());
    instance.m_database.setPort(Settings::getInstance()->value("port").toInt());
    instance.m_database.setDatabaseName(Settings::getInstance()->value("database").toString());
    instance.m_database.setUserName(Settings::getInstance()->value("user").toString());
    instance.m_database.setPassword(
                AuthProvider::instance()->decryptPassword(Settings::getInstance()->value("password").toString()));
}

void Database::writeConnectionInfo()
{
    // Save setting in a normal manner (registery in Windows, Setting file in Linux and Mac OSX)
    Settings::getInstance()->setValue("host",instance.m_database.hostName());
    Settings::getInstance()->setValue("port",instance.m_database.port());
    Settings::getInstance()->setValue("database",instance.m_database.databaseName());
    Settings::getInstance()->setValue("user",instance.m_database.userName());
    Settings::getInstance()->setValue(
                "password",AuthProvider::instance()->encryptPassword(instance.m_database.password()));
}

void Database::setBlockSize(const unsigned int &size)
{
    this->m_blockSize = size;
}

unsigned int Database::blockSize() const
{
    return this->m_blockSize;
}

void Database::backup(const QString &filename, const Database::BackupStrategy &strategy)
{
    switch(strategy)
    {
    case BinaryByVariantStrategy:
        backupByVariant(filename);
        break;
    case BinaryRuntimeCheckStrategy:
        backupByRuntimeCheck(filename);
        break;
    case BinaryCompileCheckStrategy:
        break;
    case TextBasedStrategy:
        break;
    }
}

void Database::backupByVariant(const QString &filename)
{
    QQueue<TableNode*> tablesQueue;
    getTables(tablesQueue);
    getParents(tablesQueue);
    QQueue<TableNode*> orderedQueue;
    sortTables(tablesQueue,orderedQueue);

    // Starting task
    QFile backupFile(filename);
    if(backupFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        emit backupStageChanged(tr("Analyzing database information..."));
        uint totalRows = getNumberOfDBRows();
        uint writtenRows = 0;

        QDataStream out(&backupFile);

        emit backupStageChanged(tr("Writing database information..."));
        out << static_cast<quint8>(BinaryByVariantStrategy);
        out << Database::getInstance()->database()->databaseName(); // Stored as QString
        out << quint32(orderedQueue.size());
        out << quint32(totalRows);

        emit backupStageChanged(tr("Writing data..."));
        foreach(TableNode* table, orderedQueue)
        {
            uint fieldCount = getNumberOfTableColumns(table->name);
            uint rowCount = getNumberOfTableRows(table->name);
            out << table->name;
            out << (rowCount);
            uint blockCount = rowCount/blockSize();
            if(rowCount%blockSize() != 0)
                ++blockCount;
            QString selectQueryText =  QString("SELECT * FROM %1 LIMIT %2,%3")
                    .arg(table->name,"%1",QString::number(blockSize()));
            QSqlQuery selectQuery;
            for (uint i = 0; i < blockCount; ++i)
            {
                QString selectQueryText2 = selectQueryText.arg(i*blockSize());
                selectQuery.prepare(selectQueryText2);
                if(selectQuery.exec())
                {
                    while(selectQuery.next())
                    {
                        // First strategy:
                        // Store all data in QVariant format
                        for (uint f = 0; f < fieldCount; ++f) {
                            out << selectQuery.value(f);
                        }
                        ++writtenRows;
                    }
                    emit completed(100.0*static_cast<double>(writtenRows)/static_cast<double>(totalRows));
                }
                else
                {
                }
            }
        } // end of foreach (table)
        backupFile.close();
        emit backupStageChanged(tr("Done."));
        emit completed();
    }
    else
    {
        QIO::cerr << tr("Unable to open backup file for writing.") << endl;
    }
}

void Database::backupByRuntimeCheck(const QString &filename)
{
    QQueue<TableNode*> tablesQueue;
    getTables(tablesQueue);
    getParents(tablesQueue);
    QQueue<TableNode*> orderedQueue;
    sortTables(tablesQueue,orderedQueue);

    // Starting task
    QFile backupFile(filename);
    if(backupFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        emit backupStageChanged(tr("Analyzing database information..."));
        uint totalRows = getNumberOfDBRows();
        uint writtenRows = 0;

        QDataStream out(&backupFile);

        emit backupStageChanged(tr("Writing database information..."));
        out << static_cast<quint8>(BinaryRuntimeCheckStrategy);
        out << Database::getInstance()->database()->databaseName(); // Stored as QString
        out << quint32(orderedQueue.size());
        out << quint32(totalRows);

        emit backupStageChanged(tr("Writing data..."));
        foreach(TableNode* table, orderedQueue)
        {
            uint fieldCount = getNumberOfTableColumns(table->name);
            uint rowCount = getNumberOfTableRows(table->name);

            QVector<FieldType> types;
            getTableFiledTypes(table->name,types);

            out << table->name;
            out << (rowCount);
            uint blockCount = rowCount/blockSize();
            if(rowCount%blockSize() != 0)
                ++blockCount;
            QString selectQueryText =  QString("SELECT * FROM %1 LIMIT %2,%3")
                    .arg(table->name,"%1",QString::number(blockSize()));
            QSqlQuery selectQuery;
            for (uint i = 0; i < blockCount; ++i)
            {
                QString selectQueryText2 = selectQueryText.arg(i*blockSize());
                selectQuery.prepare(selectQueryText2);
                if(selectQuery.exec())
                {
                    while(selectQuery.next())
                    {
                        // Second strategy:
                        // Store all data in native binary representation, check types at runtime
                        for (uint f = 0; f < fieldCount; ++f) {
                            switch(types[f])
                            {
                            case Type_I8:
                                out << static_cast<qint8>(selectQuery.value(f).toInt());
                                break;
                            case Type_UI8:
                                out << static_cast<quint8>(selectQuery.value(f).toUInt());
                                break;
                            case Type_I16:
                                out << static_cast<qint16>(selectQuery.value(f).toInt());
                                break;
                            case Type_UI16:
                                out << static_cast<quint16>(selectQuery.value(f).toUInt());
                                break;
                            case Type_I32:
                                out << static_cast<qint32>(selectQuery.value(f).toInt());
                                break;
                            case Type_UI32:
                                out << static_cast<quint32>(selectQuery.value(f).toUInt());
                                break;
                            case Type_I64:
                                out << static_cast<qint64>(selectQuery.value(f).toInt());
                                break;
                            case Type_UI64:
                                out << static_cast<quint64>(selectQuery.value(f).toUInt());
                                break;
                            case Type_BOOL:
                                out << selectQuery.value(f).toBool();
                                break;
                            case Type_TEXT:
                                out << QString::fromUtf8(selectQuery.value(f).toByteArray());
                                break;
                            case Type_FLOAT:
                                out << selectQuery.value(f).toFloat();
                                break;
                            case Type_DOUBLE:
                                out << selectQuery.value(f).toDouble();
                                break;
                            case Type_DATE_TIME:
                                out << selectQuery.value(f).toDateTime();
                                break;
                            case Type_DATE:
                                out << selectQuery.value(f).toDate();
                                break;
                            case Type_TIME:
                                out << selectQuery.value(f).toTime();
                                break;
                            }
                        }
                        ++writtenRows;
                    }
                    emit completed(100.0*static_cast<double>(writtenRows)/static_cast<double>(totalRows));
                }
                else
                {
                }
            }
        } // end of foreach (table)
        backupFile.close();
        emit backupStageChanged(tr("Done."));
        emit completed();
    }
    else
    {
        QIO::cerr << tr("Unable to open backup file for writing.") << endl;
    }
}

void Database::restore(const QString &filename)
{
    QFile backupFile(filename);
    if(backupFile.open(QIODevice::ReadOnly))
    {
        QDataStream in(&backupFile);
        QString schemaName;
        quint32 tableCount, totalRows, restoredRecords = 0;
        quint8 rawBackupType;
        in >> rawBackupType;
        BackupStrategy backupType = static_cast<BackupStrategy>(rawBackupType);
        in >> schemaName;
        in >> tableCount >> totalRows;
#ifdef DEBUG
        qDebug() << schemaName << tableCount << totalRows;
#endif
        if(schemaName != database()->databaseName())
        {
            QIO::cerr << tr("Database names mismatch") << endl;
            return;
        }
        for(quint32 t=0; t<tableCount; ++t)
        {
            switch(backupType)
            {
            case BinaryByVariantStrategy:
                restoreByVariant(in,totalRows, restoredRecords);
                break;
            case BinaryRuntimeCheckStrategy:
                break;
            case BinaryCompileCheckStrategy:
                break;
            case TextBasedStrategy:
                break;
            }
        }
    }
    else
    {
        QIO::cerr << tr("Unable to open backup file for reading.") << endl;
    }
}


uint Database::getNumberOfDBRows()
{
    QSqlQuery prepareData;
    if(!prepareData.exec("CALL COUNT_ALL_RECORDS_BY_TABLE"))
    {
        QIO::cerr << tr("Unable to call stored procedure `CALL COUNT_ALL_RECORDS_BY_TABLE' "
                        "in order to get count of total records of database:")
                  << endl;
        QIO::cerr << prepareData.lastError().text() << endl;
        return 0;
    }
    QSqlQuery getData;
    if( getData.exec("SELECT SUM(RECORD_COUNT) AS TOTAL_DATABASE_RECORD_CT FROM TCOUNTS"))
    {
        getData.next();
        uint result = getData.value(0).toUInt();
        return result;
    }
    return 0;
}

uint Database::getNumberOfTableRows(const QString &tableName)
{
    QString countQueryText = QString("SELECT COUNT(*) from %1").arg(tableName);
    QSqlQuery countQuery;
    countQuery.prepare(countQueryText);
    if(countQuery.exec())
    {
        countQuery.next();
        uint result = countQuery.value(0).toUInt();
        return result;
    }
    else
    {
        QIO::cerr << tr("Unable to fetch number of records in table `%1'").arg(tableName) << endl;
    }
    return 0;
}

uint Database::getNumberOfTableColumns(const QString &tableName)
{
    QString selectFieldCountText = QString(
                "SELECT COUNT(*) FROM information_schema.`COLUMNS`"
                "WHERE table_name = '%1'"
                "AND TABLE_SCHEMA = DATABASE()").arg(tableName);
    QSqlQuery selectFieldCount;
    selectFieldCount.prepare(selectFieldCountText);
    selectFieldCount.exec();
    selectFieldCount.next();
    uint fieldCount = selectFieldCount.value(0).toUInt();
    return fieldCount;
}

void Database::getTableFiledTypes(const QString &tableName, QVector<FieldType> &types)
{
    QSqlQuery selectFieldType;
    selectFieldType.prepare(QString("describe %1").arg(tableName));
    if(!selectFieldType.exec())
    {
        QIO::cerr << tr("Unable to get types of fields for table `%1'").arg(tableName) << endl;
        QIO::cerr << selectFieldType.lastError().text() << endl;
        return;
    }
    types.clear();
    int index = selectFieldType.record().indexOf("Type");
#ifdef DEBUG
    qDebug() << "Types of table: " << tableName;
#endif
    while(selectFieldType.next())
    {
        QString typeDescription = selectFieldType.value(index).toString();
#ifdef DEBUG
        qDebug() << typeDescription;
#endif
        QRegExp typeExpression;
        typeExpression.setCaseSensitivity(Qt::CaseInsensitive);
        typeExpression.setPattern("BIT(\\(1\\))?|TINYINT\\(1\\)\\s*(unsigned)?|BOOL|BOOLEAN");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_BOOL;
#ifdef DEBUG
            qDebug() << "Type_BOOL";
#endif
            continue;
        }
        typeExpression.setPattern("TINYINT(\\(\\d\\))?");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_I8;
#ifdef DEBUG
            qDebug() << "Type_I8";
#endif
            continue;
        }
        typeExpression.setPattern("TINYINT(\\(\\d\\))?\\s*(unsigned)");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_UI8;
#ifdef DEBUG
            qDebug() << "Type_UI8";
#endif
            continue;
        }

        typeExpression.setPattern("SMALLINT(\\(\\d\\))?");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_I16;
#ifdef DEBUG
            qDebug() << "Type_I16";
#endif
            continue;
        }
        typeExpression.setPattern("SMALLINT(\\(\\d\\))?\\s*(unsigned)|YEAR\\(2|4\\)");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_UI16;
#ifdef DEBUG
            qDebug() << "Type_UI16";
#endif
            continue;
        }

        typeExpression.setPattern("(MEDIUMINT|INTEGER|INT)(\\(\\d\\))?");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_I32;
#ifdef DEBUG
            qDebug() << "Type_I32";
#endif
            continue;
        }
        typeExpression.setPattern("(MEDIUMINT|INTEGER|INT)(\\(\\d\\))?\\s*(unsigned)");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_UI32;
#ifdef DEBUG
            qDebug() << "Type_UI32";
#endif
            continue;
        }

        typeExpression.setPattern("BIGINT(\\(\\d\\))?");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_I64;
#ifdef DEBUG
            qDebug() << "Type_I64";
#endif
            continue;
        }
        typeExpression.setPattern("BIGINT(\\(\\d\\))?\\s*(unsigned)");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_UI64;
#ifdef DEBUG
            qDebug() << "Type_UI64";
#endif
            continue;
        }
        // Text data
        typeExpression.setPattern("(CHAR|VARCHAR|BINARY|VARBINARY|BLOB|TEXT|TINYTEXT|MEDIUMTEXT|LONGTEXT|ENUM|SET).*");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_TEXT;
#ifdef DEBUG
            qDebug() << "Type_TEXT";
#endif
            continue;
        }
        // Temporal data
        typeExpression.setPattern("DATE");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_DATE;
#ifdef DEBUG
            qDebug() << "Type_DATE";
#endif
            continue;
        }
        typeExpression.setPattern("DATETIME|TIMESTAMP");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_DATE_TIME;
#ifdef DEBUG
            qDebug() << "Type_DATE_TIME";
#endif
            continue;
        }
        typeExpression.setPattern("TIME");
        if(typeExpression.exactMatch(typeDescription))
        {
            types << Type_TIME;
#ifdef DEBUG
            qDebug() << "Type_TIME";
#endif
            continue;
        }
    }
}

void Database::getTables(QQueue<TableNode *> &tables)
{
    QSqlQuery fetchTables;
    fetchTables.prepare("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES where TABLE_SCHEMA=DATABASE()");
    if(fetchTables.exec())
    {
        while(fetchTables.next())
        {
            TableNode* item = new TableNode(fetchTables.value(0).toString(),-1);
            tables << item;
        }
    }
}

void Database::getParents(const QQueue<TableNode *> &inputList)
{
    QSqlQuery getParents;
    QFile queryFile(":/data/resources/mysql_fk_fetch.sql");
    if(!queryFile.open(QFile::ReadOnly | QFile::Text))
    {
        QIO::cerr << tr("Unable to open SQL query file") << endl;
        return;
    }
    QString genericQueryText = QString::fromUtf8(queryFile.readAll());
    foreach(TableNode* item, inputList)
    {
        QString queryText = genericQueryText.arg(item->name);
        getParents.prepare(queryText);
        if(getParents.exec())
        {
            while(getParents.next())
            {
                QString tableName = getParents.value(0).toString();
                foreach(TableNode* parentItem, inputList)
                {
                    if(parentItem->name == tableName)
                    {
                        item->referencedTables << parentItem;
                    }
                }
            }
        }
        else
        {
            QIO::cerr << tr("Unable to execute statement") << endl;
            QIO::cerr << getParents.lastError().text() << endl;
            // TODO: throw new exception
        }
    }
}

void Database::sortTables(QQueue<TableNode *> &input, QQueue<TableNode *> &output)
{
    while(! input.isEmpty())
    {
        // Check dependencies
        bool satisfied = true;
        foreach(TableNode* parent, input.head()->referencedTables)
        {
            if(parent->degreeOfFreedom == -1)
            {
                satisfied = false;
                break;
            }
            else
            {

            }
        }
        if(satisfied)
        {
            int max =0;
            foreach(TableNode* parent, input.head()->referencedTables)
                max = (max < parent->degreeOfFreedom+1) ? parent->degreeOfFreedom+1 : max;
            input.head()->degreeOfFreedom = max;
            output.enqueue(input.dequeue());
        }
        else
        {
            input.enqueue(input.dequeue());
        }
    }

}

void Database::restoreByVariant(QDataStream& in, const quint32& totalRows, quint32& restoredRecords)
{
    QString tableName;
    quint32 rowCount;
    in >> tableName >> rowCount;
    quint32 columnsCount = getNumberOfTableColumns(tableName);
    for(quint32 r=0; r<rowCount; ++r)
    {
        for(uint c=0; c<columnsCount; ++c)
        {
            QVariant value;
            in >> value;
            // TODO: add restoring mechanism
        }
    }
}
