//-----------------------------------------------------------------------------
// Copyright 2021 ADSC
//
// This file is part of the OpenPLC61850 Software Stack.
//
// OpenPLC61850 is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// OpenPLC61850 is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with OpenPLC61850.  If not, see <http://www.gnu.org/licenses/>.
//------
//
// Mapper tool - maps IEC61850 data objects to variable/addresses in OpenPLC Runtime
//-----------------------------------------------------------------------------

#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdio.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "pugixml.hpp"

#define DEVICE_DELIM "/"
#define DELIMITER "."
#define NO_TARGET 0
#define CLIENT 1
#define SERVER 2

char *stfilename;

int numSCL;
std::vector<char *> sclfilenames;

bool hasOutfile;
char *outfilename;

int target;

std::unordered_map<std::string, std::string> var_addr_map;

std::vector<std::string> ipaddr_buffer;
std::vector<std::vector<std::string>> report_dataset_buffer;
std::vector<std::string> controlVariables;
std::vector<std::string> mapvaraddr_buffer;

using namespace pugi;

/*
    Trims leading and trailing whitespaces from string
*/
std::string trimWhitespace(std::string line)
{
    int startIndex = line.find_first_not_of(" \n\r\t");
    int endIndex = line.find_last_not_of(" \n\r\t");
    int count = endIndex - startIndex + 1;

    if (startIndex < 0 || endIndex < 0) {
        return line;
    }

    return line.substr(startIndex, count);
}

/*
    Function to get IP address from SCL file.
    Called only for client target.
*/
void get_IP_address(xml_document &sclfile)
{
    xml_node conap = sclfile.child("SCL").child("Communication").child("SubNetwork").child("ConnectedAP");
    std::string iedName = conap.attribute("iedName").value();
    for (xml_node ip = conap.child("Address").child("P"); ip; ip = ip.next_sibling("P")) {
        std::string iptype = ip.attribute("type").value();
        if (iptype.compare("IP") == 0) {
            ipaddr_buffer.push_back((std::string)ip.first_child().text().as_string());
        }
    }
}

/*
    Called by get_report_dataset()
    Function to get dataset details from SCL file (assume only 1 logical device).
*/

std::string get_dataSet_reference(xml_document &sclfile, std::string given_name)
{
    xml_node phydev = sclfile.child("SCL").child("IED");
    std::string pdld_name = phydev.attribute("name").value();

    xml_node logdev = phydev.child("AccessPoint").child("Server").child("LDevice");
    pdld_name = pdld_name + logdev.attribute("inst").value();

    for (xml_node dataset = logdev.child("LN0").child("DataSet"); dataset; dataset = dataset.next_sibling("DataSet")) {
        std::string dataset_name = dataset.attribute("name").value();
        if (dataset_name.compare(given_name) == 0) {
            return (pdld_name + "/LLN0$" + dataset_name);
        }
        
    }

    return "X";
}

/*
    Function to get report details from SCL file (assume only 1 logical device).
    Called only for client target.
*/

void get_report_dataset(xml_document &sclfile)
{
    std::vector<std::string> reports_datasets;
    xml_node phydev = sclfile.child("SCL").child("IED");
    std::string pdld_name = phydev.attribute("name").value();

    xml_node logdev = phydev.child("AccessPoint").child("Server").child("LDevice");
    pdld_name = pdld_name + logdev.attribute("inst").value();

    for (xml_node report = logdev.child("LN0").child("ReportControl"); report; report = report.next_sibling("ReportControl")) {
        std::string report_name = report.attribute("name").value();
        std::string dataset_ref = get_dataSet_reference(sclfile, report.attribute("datSet").value()); 
        
        int numInstances = std::stoi(report.child("RptEnabled").attribute("max").value());
        for(int i = 1; i <= numInstances; i++) {
            char inst[10];
            sprintf(inst, "%02d", i);
            reports_datasets.push_back(pdld_name + "/LLN0.RP." + report_name + inst + " " + dataset_ref);
        }
    }

    report_dataset_buffer.push_back(reports_datasets);
}

/* 
    Called by get_scl_variables().
    Recursive function to extract PLC variables of data attributes from supplied parent node.
    Parent node can be of DataObject or DataAttribute.
*/
void getVarAddrMapping(xml_node parent, std::string pathstring)
{
    if (parent.child("Private")) {
        for (xml_node priv_node = parent.child("Private"); priv_node; priv_node = priv_node.next_sibling("Private")) {
            std::string newpathstring = pathstring + DELIMITER + priv_node.attribute("name").value();
            getVarAddrMapping(priv_node, newpathstring);
        }
    }
    else if (parent.child("Property")) {
        for (xml_node prop_node = parent.child("Property"); prop_node; prop_node = prop_node.next_sibling("Property")) {
            std::string prop_name = prop_node.attribute("Name").value();
            std::string prop_value = prop_node.attribute("Value").value();
            if (prop_name.compare("sMonitoringVar") == 0 && !prop_value.empty()) {
                std::string address = var_addr_map[prop_value];
                if (address.empty()) {
                    address = "X";
                }
                if (target == CLIENT) {
                    mapvaraddr_buffer.push_back("MONITOR " + pathstring + " " + address);
                }
                else if (target == SERVER) {
                    mapvaraddr_buffer.push_back("MONITOR " + pathstring + " " + address);
                }
            }
            else if (prop_name.compare("sControlVar") == 0 && !prop_value.empty()) {
                std::string address = var_addr_map[prop_value];
                if (address.empty()) {
                    address = "X";
                }
                if (target == CLIENT) {
                    mapvaraddr_buffer.push_back("CONTROL " + pathstring + " " + address);
                    controlVariables.push_back(pathstring + " " + address);
                }
                else if (target == SERVER) {
                    mapvaraddr_buffer.push_back("CONTROL " + pathstring + " " + address);
                }
            }
        }
    }
}

/* 
    Function to map IEC61850 variables to Modbus address from SCL file.
    Called for both server and client targets.
*/
void get_scl_variables(xml_document &sclfile)
{
    xml_node dtt = sclfile.child("SCL").child("DataTypeTemplates");

    xml_node ied = sclfile.child("SCL").child("IED");
    std::string ied_name = ied.attribute("name").value();

    xml_node ld = ied.child("AccessPoint").child("Server").child("LDevice");
    std::string ld_name = ld.attribute("inst").value();

    std::unordered_map<std::string, std::string> lntype_lninst_mapping;
    
    //Get map of ln type -> ln instance
    for (xml_node ln_node = ld.child("LN"); ln_node; ln_node = ln_node.next_sibling("LN")) {
        std::string ln_type = ln_node.attribute("lnType").value();
        std::string ln_class = ln_node.attribute("lnClass").value();
        std::string ln_inst = ln_node.attribute("inst").value();

        lntype_lninst_mapping[ln_type] = ln_class + ln_inst;
    }

    std::unordered_map<std::string, std::string> do_ln_mapping;

    //Get map of do_type -> (ln_name + do_name)
    for (xml_node ln_node = dtt.child("LNodeType"); ln_node; ln_node = ln_node.next_sibling("LNodeType")) {
        std::string ln_type = ln_node.attribute("id").value();

        for (xml_node do_node = ln_node.child("DO"); do_node; do_node = do_node.next_sibling("DO")) {
            xml_attribute do_name = do_node.attribute("name");
            xml_attribute do_type = do_node.attribute("type");
            std::string do_namestring = do_name.value();

            do_ln_mapping[do_type.value()] = ied_name + ld_name + DEVICE_DELIM + lntype_lninst_mapping[ln_type] + DELIMITER + do_namestring;
        }
    }

    //Path to get var names: dtt > dotype > da > private [ > private]* > property (sMonitoringVar/scontrolVar)
    for (xml_node do_node = dtt.child("DOType"); do_node; do_node = do_node.next_sibling("DOType")) {
        xml_attribute do_name = do_node.attribute("id");
        std::string do_namestring = do_name.value();

        for (xml_node da_node = do_node.child("DA"); da_node; da_node = da_node.next_sibling("DA")) {
            std::string pathstring = do_ln_mapping[do_namestring];
            getVarAddrMapping(da_node, pathstring);
        }
    }
}

int process_scl_files()
{
    for (auto it : sclfilenames) {
        xml_document sclfile;
        xml_parse_result result = sclfile.load_file(it);

        if (result.status == status_file_not_found) {
            std::cout << "Failed to open SCL file " << it << "!\n";
            return 1;
        }
        else if (result.status == status_io_error) {
            std::cout << "Failed to read SCL file" << it << "!\n";
            return 1;
        }
        else if (result.status != status_ok) {
            std::cout << "Error parsing SCL file" << it << ": " << result.status << "!\n";
            return 1;
        }
        else {
            std::cout << "Parsing SCL file" << it << "\n";
        }

        get_scl_variables(sclfile);

        if (target == CLIENT) {
            get_IP_address(sclfile);
            get_report_dataset(sclfile);
            controlVariables.push_back("");
        }
    }

    return 0;
}

/* 
    Process ST file to map PLC variable names to Modbus address.
    Called for both server and client targets.
*/
int process_st_file()
{
    std::ifstream stinput(stfilename);
    if (!stinput) {
        std::cout << "Failed to open ST file!\n";
        return 1;
    }
    std::cout << "Parsing ST file\n";

    std::string line;
    bool inVar = false;
    std::regex reg("[a-zA-Z0-9_]+ AT %[A-Z0-9.]+ : [A-Z0]+;");

    while (std::getline(stinput, line)) {
        line = trimWhitespace(line);

        if (inVar == false) {
            if (line.compare("VAR") == 0) {
                inVar = true;
            }
            continue;
        }

        if (inVar && std::regex_match(line, reg)) {
            std::stringstream ss(line);
            std::string token;

            std::getline(ss, token, ' ');
            //std::cout << "Var_name: " << token << "\n";
            std::string varName = token;

            std::getline(ss, token, ' '); //get rid of "AT"
            std::getline(ss, token, ' ');
            //std::cout << "Address: " << token << "\n";
            std::string address = token;

            var_addr_map[varName] = address;
        }

        if (inVar && line.compare("END_VAR") == 0) {
            inVar = false;
            continue;
        }
    }

    stinput.close();

    return 0;
}

int print_command_error()
{
    std::cout << "Invalid command!\n";
    std::cout << "Usage:\nmapper <-server|-client> -st <ST-filename> -scl <SCL-filename> [SCL-filename ...] [-o <output-filename>]\n";

    return 1;
}

int process_args(int argc, char *argv[])
{

    if (argc < 6) {
        return print_command_error();
    }

    bool inST = false;
    bool inSCL = false;
    bool inOutput = false;
    numSCL = 0;
    target = NO_TARGET;

    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];

        if (inST) {
            stfilename = arg;
            inST = false;
            continue;
        }
        if (inOutput) {
            outfilename = arg;
            hasOutfile = true;
            inOutput = false;
            continue;
        }
        if (inSCL) {
            if (strncmp(arg, "-", 1) == 0) {
                inSCL = false;
            }
            else {
                sclfilenames.push_back(arg);
            }
        }

        if (!inST && !inSCL && !inOutput) {
            if (strncmp(arg, "-st", 3) == 0) {
                inST = true;
            }
            else if (strncmp(arg, "-scl", 4) == 0) {
                inSCL = true;
            }

            else if (strncmp(arg, "-o", 2) == 0) {
                inOutput = true;
            }

            else if (strncmp(arg, "-server", 7) == 0) {
                if (target > 0) {
                    return print_command_error();
                }
                target = SERVER;
            }

            else if (strncmp(arg, "-client", 7) == 0) {
                if (target > 0) {
                    return print_command_error();
                }
                target = CLIENT;
            }

            else {
                return print_command_error();
            }
        }
    }

    if (target == NO_TARGET) {
        return print_command_error();
    }

    if (target == SERVER && sclfilenames.size() > 1) {
        std::cout << "Only 1 SCL file allowed for server target!\n";
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (process_args(argc, argv)) {
        exit(1);
    }

    std::cout << std::endl;
    std::cout << "ST file: " << stfilename << std::endl;
    std::cout << "SCL files: ";
    for (auto it : sclfilenames) {
        std::cout << it << " ";
    }
    std::cout << std::endl;
    if (hasOutfile)
        std::cout << "Outfile: " << outfilename << std::endl;
    std::cout << std::endl;

    std::ofstream outfile;
    std::streambuf *buf;

    if (hasOutfile) {
        outfile = std::ofstream(outfilename);
        if (!outfile) {
            std::cout << "Failed to open/create output file!\n";
            return 1;
        }
        buf = outfile.rdbuf();
    }
    else {
        buf = std::cout.rdbuf();
    }

    //set stream to outfile or std::cout
    std::ostream out(buf);

    //Process st file
    if (process_st_file()) {
        return 1;
    }

    if (process_scl_files()) {
        return 1;
    }

    //output
    
    if (target == SERVER) {
        if (!hasOutfile)
            std::cout << "\nIEC61850 Variables to ModbusAddress Mapping:\n";
        for (auto it : mapvaraddr_buffer) {
            out << it << std::endl;
        }
    }
    else if (target == CLIENT) {
        if (!hasOutfile)
            std::cout << "\nIED Details:\n";
        int j = 0;
        for (int i = 0; i < sclfilenames.size() && ipaddr_buffer.size() > 0; i++) {
            out << ipaddr_buffer.at(i) << std::endl;
            for (auto report_dataset: report_dataset_buffer.at(i)) {
                out << report_dataset << std::endl;
            }
            for (j; j < controlVariables.size(); j++) {
                std::string line = controlVariables.at(j);
                if (line.empty()) {
                    j++;
                    break;
                }
                out << line << std::endl;
            }
        }

        out << std::endl;
        if (!hasOutfile)
            std::cout << "\nIEC61850 Variables to ModbusAddress Mapping:\n";
        for (auto it : mapvaraddr_buffer) {
            out << it << std::endl;
        }
    }

    if (hasOutfile) {
        outfile.close();
    }

    std::cout << "Mapping done, exit\n\n";

    return 0;
}